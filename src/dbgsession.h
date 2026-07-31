#pragma once
// DbgSession glues DbgHost to two live input sources - the GUI's own command
// bar and an external named pipe - so both a human and an outside script can
// drive (and watch) the same debug session at once.
//
// All DbgEng calls happen on one dedicated worker thread (the engine is not
// meant to be called concurrently from multiple threads). Everything else
// (the GUI render thread, the pipe thread) only ever touches copies of
// `Snapshot`, taken under `stateMutex`, or enqueues a command and waits on a
// promise for its result.

#include "dbghost.h"
#include "cmddispatch.h"
#include "plugin_manager.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

struct StackEntry {
    ULONG64 addr = 0;
    ULONG64 value = 0;
    std::wstring symbol;
};

// One tab in the CPU window. Unlike DbgHost::Processes() - which only knows
// what is alive right now - a tab outlives its process: when a debuggee exits
// (or is killed) the tab stays, flagged dead, until the user closes it, so you
// can still read the disassembly that was on screen when it died.
struct ProcTab {
    ULONG       engineId = 0;
    ULONG       pid = 0;
    std::string name;
    bool        alive = true;
};

struct Snapshot {
    bool sessionActive = false;
    bool stopped = false;      // waiting at a breakpoint/step/launch-break

    RegFile regs;

    ULONG64 disasmViewAddr = 0;
    std::vector<DisasmLine> disasmLines;

    ULONG64 dumpViewAddr = 0;
    DumpWidth dumpWidth = DumpWidth::Byte;
    std::vector<unsigned char> dumpBytes;

    std::vector<StackEntry> stack;

    // Breakpoints the engine currently holds, so the CPU window can paint
    // their rows red and clear one by clicking it.
    std::vector<BpInfo> breakpoints;

    // Processes in this session, one CPU tab each, plus which one the
    // register/disasm/stack data above belongs to.
    std::vector<ProcTab> processes;
    ULONG currentProcEngineId = 0;
    // Bumped every time a process is added to the list; the GUI snaps its tab
    // selection to `focusProcEngineId` whenever it sees this number change, so
    // a spawned child pulls focus exactly once instead of every frame.
    ULONG procSerial = 0;
    ULONG focusProcEngineId = 0;

    // Command line of the last target launched in this session, whatever
    // started it (Open dialog, command bar, pipe). The GUI persists it as
    // "remember last opened executable".
    std::string lastTarget;

    std::vector<std::string> log;  // capped ring of recent event/command lines

    // OllyDbg-style option state, mirrored here so the GUI can render the
    // Options menu / Exceptions page without touching the engine off-thread.
    bool optChildDbg = false;
    bool optBreakModule = false;
    bool optBreakThread = false;
    std::vector<ExcRange> ignoredExceptions;
    std::vector<unsigned long> seenExceptions;
};

class DbgSession {
public:
    DbgSession();
    ~DbgSession();

    // Enqueue a command from the GUI's own command bar; fire-and-forget
    // (the result shows up in the log, same as any other source).
    void PushCommand(const std::wstring& text);

    // Enqueue a command and block the calling thread for the result - used
    // by the named-pipe handler to give external callers a request/response
    // channel.
    std::string PushCommandBlocking(const std::wstring& text);

    Snapshot GetSnapshot();

    // Thread-safe engine access for plugin host exports (Odbg_* in
    // odbg_plugin_sdk.h). Callable from any thread: runs directly if already
    // on the worker thread, otherwise marshals onto it and blocks for the
    // result - the same rule that fixed the original Init()-on-wrong-thread
    // bug applies to every plugin call too.
    template <class F>
    auto RunOnWorker(F&& fn) -> decltype(fn()) {
        if (std::this_thread::get_id() == m_workerThreadId) return fn();
        using R = decltype(fn());
        std::promise<R> prom;
        auto fut = prom.get_future();
        {
            std::lock_guard<std::mutex> lk(m_jobMutex);
            m_jobs.push_back([&fn, &prom]() {
                if constexpr (std::is_void_v<R>) { fn(); prom.set_value(); }
                else { prom.set_value(fn()); }
            });
        }
        return fut.get();
    }

    bool SafeReadMemory(ULONG64 addr, void* buf, ULONG size);
    bool SafeWriteMemory(ULONG64 addr, const void* buf, ULONG size);
    ULONG64 SafeGetRegister(const wchar_t* name);
    bool SafeSetRegister(const wchar_t* name, ULONG64 value);
    RegFile SafeGetRegisters();
    int SafeAddBreakpoint(const std::wstring& expr);
    bool SafeRemoveBreakpoint(int id);
    ULONG64 SafeGetPeb();
    void SafeGo();
    void SafePause();
    void SafeStepInto();
    void SafeStepOver();
    bool IsSessionActive();
    bool IsStopped();
    void LogFromPlugin(const std::string& line);

    PluginManager& Plugins() { return m_plugins; }

private:
    struct QueuedCmd {
        std::wstring text;
        std::promise<std::string>* promise = nullptr;
    };

    void WorkerMain();
    void PipeMain();
    void HandleCommand(const QueuedCmd& cmd);
    void HandleEvent(const BreakEvent& ev);
    // Refreshes the disasm/dump/stack/register snapshot after a stop and returns
    // the register file to hand to plugins. Call with m_stateMutex held. It does
    // NOT fire plugins - the caller must do that AFTER releasing the mutex, since
    // a plugin's Odbg_Paused may call back into Odbg_Log/Odbg_* which re-lock.
    OdbgRegs RefreshAfterStop_NoLock();
    void RefreshDisasmView_NoLock();
    void RefreshDumpView_NoLock();
    void RefreshStackView_NoLock();
    void AppendLog_NoLock(const std::string& line);
    // Copy the engine's current option state into m_state so the GUI can render
    // it. Call with m_stateMutex held.
    void SyncOptions_NoLock();
    // Merge the engine's live process list into the CPU window's tab list,
    // keeping exited processes as dead tabs. Only valid while stopped (naming a
    // process switches the engine's current process). Call with m_stateMutex held.
    void SyncProcesses_NoLock();

    DbgHost m_host;
    std::atomic<bool> m_quit{false};
    std::thread::id m_workerThreadId;

    std::mutex m_qMutex;
    std::deque<QueuedCmd> m_queue;

    std::mutex m_jobMutex;
    std::deque<std::function<void()>> m_jobs;

    std::mutex m_stateMutex;
    Snapshot m_state;

    PluginManager m_plugins;

    std::thread m_worker;
    std::thread m_pipe;
};
