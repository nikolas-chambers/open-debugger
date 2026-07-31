#pragma once
// Minimal RAII wrapper around the DbgEng COM interfaces (dbgeng.dll).
// Gives a scriptable debugger core: launch/attach, deferred breakpoints,
// module-load break, registers, memory, disasm, symbols.

#include <windows.h>
#include <DbgEng.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

template <class T>
class ComPtr {
public:
    ComPtr() : m_p(nullptr) {}
    ~ComPtr() { reset(); }
    void reset(T* p = nullptr) {
        if (m_p) m_p->Release();
        m_p = p;
    }
    T* get() const { return m_p; }
    T* detach() { T* t = m_p; m_p = nullptr; return t; }
    T** operator&() { return &m_p; }
    T* operator->() const { return m_p; }
    ComPtr& operator=(T* p) { reset(p); return *this; }
    explicit operator bool() const { return m_p != nullptr; }
private:
    T* m_p;
};

// Registers we usually care about on x64.
struct RegFile {
    ULONG64 rax = 0, rbx = 0, rcx = 0, rdx = 0, r8 = 0, r9 = 0;
    ULONG64 r10 = 0, r11 = 0, rsp = 0, rbp = 0, rip = 0, rdi = 0, rsi = 0;
};

// Kinds of event that paused us.
enum class StopReason {
    None,
    Breakpoint,
    ModuleLoaded,
    Exception,
    ProcessExited,
    Step,          // single-step (StepInto/StepOver) completed
    ThreadCreated, // new thread started (break-on-new-thread option)
};

struct BreakEvent {
    StopReason reason = StopReason::None;
    ULONG      bpId = 0;
    ULONG64    offset = 0;
    std::wstring module;      // for ModuleLoaded
    ULONG64    moduleBase = 0;
    ULONG      exceptionCode = 0;
};

// One decoded instruction.
struct DisasmLine {
    ULONG64      addr = 0;       // instruction address
    ULONG64      next = 0;       // address of the following instruction
    std::wstring header;         // "MODULE!Symbol+0xNN"
    std::wstring bytes;          // raw instruction bytes, hex
    std::wstring text;           // mnemonic + operands
};

// An ignored-exception entry: a single code when lo==hi, otherwise an inclusive
// range (OllyDbg lets you ignore a whole span, e.g. 0x40000000-0x4000FFFF).
struct ExcRange {
    unsigned long lo = 0;
    unsigned long hi = 0;
};

// One live breakpoint as the engine currently holds it. The front-end needs
// address + id to paint breakpoint rows in the disassembly and to clear one
// by clicking it (which it does by id, via `bc`).
struct BpInfo {
    ULONG   id = 0;
    ULONG64 addr = 0;
    bool    enabled = false;
};

// One process in the debug session. With child-following on there can be
// several at once (a bootstrapper plus the real application it launches, ...),
// and the CPU window gives each its own tab.
struct ProcInfo {
    ULONG       engineId = 0;   // DbgEng's per-session process id (what you pass to SetCurrentProcessId)
    ULONG       pid = 0;        // OS process id
    std::string name;           // executable file name, no path
};

// A symbol overlay entry, absolute address -> name.
struct OverlaySym {
    ULONG64      addr = 0;
    ULONG64      size = 0;
    std::wstring name;
};

class DbgHost {
public:
    DbgHost();
    ~DbgHost();

    // Initialize the engine; returns false on failure.
    bool Init();
    void Shutdown();

    void SetSymbolPath(const std::wstring& path);
    void ReloadSymbols();
    void SetVerbose(bool v) { m_verbose = v; }

    // Launch a fresh target. cmdline is the full command line.
    bool Launch(const std::wstring& cmdline);

    // Attach to a running process by PID.
    bool Attach(DWORD pid);

    // Kill every debuggee and tear the session down (the toolbar's X). Same
    // engine call as EndSession(), named for what the user asked for.
    void Kill() { EndSession(); }

    // Re-run whatever this session was started from (the toolbar's rewind):
    // kills the current target, then repeats the original Launch/Attach.
    // Breakpoints do not survive - they belong to the dead session.
    bool Restart();
    // What this session was started from, for the GUI's "remember last target".
    const std::wstring& LastLaunchCmdline() const { return m_lastLaunch; }
    DWORD LastAttachPid() const { return m_lastAttachPid; }

    // Child-process following (OllyDbg's "Debug child processes"): when on, any
    // process the debuggee spawns is also debugged - it stops at its own loader
    // breakpoint so you can set breakpoints in it before it runs. Backed by
    // DbgEng's .childdbg setting; applied immediately if the engine is up and
    // re-applied on each Launch. A launcher that relaunches itself into a child
    // and exits is invisible without this - you only ever see the stub.
    void SetFollowChildren(bool on);
    bool FollowChildren() const { return m_followChildren; }
    // Number of processes currently in the debug session (used to keep the
    // session alive when the parent exits but a followed child is still live).
    ULONG NumProcesses();

    // Every process in the session, for the CPU window's per-process tabs.
    // Only meaningful while stopped: naming a process means briefly making it
    // current, which is only safe when the engine is in a break state.
    std::vector<ProcInfo> Processes();
    // Engine id of the process the register/disasm/memory calls apply to.
    ULONG CurrentProcessEngineId();
    // Switch which process is current (clicking another CPU tab).
    bool SetCurrentProcess(ULONG engineId);

    // Ignored exceptions (OllyDbg's Options > Exceptions "ignore" list): first-
    // chance exception codes - single or a range - the debugger passes straight
    // through to the debuggee's own handler instead of stopping on. Execution
    // continues transparently - WaitForEvent never even surfaces an ignored one.
    void AddIgnoredException(ULONG code) { AddIgnoredExceptionRange(code, code); }
    void AddIgnoredExceptionRange(ULONG lo, ULONG hi);
    void RemoveIgnoredExceptionAt(size_t index);   // by row (GUI remove button)
    void RemoveIgnoredException(ULONG code);        // remove any range == {code,code}
    bool IsExceptionIgnored(ULONG code) const;
    const std::vector<ExcRange>& IgnoredExceptions() const { return m_ignoredExceptions; }

    // Exceptions seen this session (OllyDbg shows what has actually fired so you
    // can one-click add it to the ignore list). Unique codes, first-seen order.
    const std::vector<ULONG>& SeenExceptions() const { return m_seenExceptions; }
    void RecordSeenException(ULONG code);

    // Clears engine-level session state after a target has exited, so the
    // same IDebugClient can Launch/Attach again. Without this, a subsequent
    // Attach can fail (observed: AttachProcess returning 0xd0000001) because
    // the engine still considers the previous (now-dead) process current.
    void EndSession();

    // Add a breakpoint by "module!symbol" expression, e.g.
    // L"kernel32!CreateFileW". Resolves immediately via the PE
    // export table if the module is loaded, otherwise defers until the module
    // loads. Returns breakpoint id or -1.
    int AddDeferredBreakpoint(const std::wstring& expr);
    // Add a breakpoint at an absolute address.
    int AddOffsetBreakpoint(ULONG64 offset);

    // Resolve module!symbol to an absolute address using the module's PE
    // export table in the debuggee's memory. Returns false if unresolved.
    bool ResolveExport(const std::wstring& module, const std::wstring& symbol, ULONG64& out);
    // Try to arm any pending module!symbol breakpoints for the just-loaded
    // module. Called from the module-load callback.
    void ResolvePendingForModule(const std::wstring& moduleName, ULONG64 moduleBase);

    // Set a module-load break: pauses when a module whose name contains this
    // substring loads. "" disables.
    void SetModuleBreak(const std::wstring& name);

    // OllyDbg "Break on new module (DLL)": pause on every module load, not just
    // one matching the substring filter above.
    void SetBreakOnModuleLoad(bool on) { m_breakOnModuleLoad = on; }
    bool BreakOnModuleLoad() const { return m_breakOnModuleLoad; }
    // OllyDbg "Break on new thread": pause when the debuggee starts a thread.
    void SetBreakOnThreadCreate(bool on) { m_breakOnThreadCreate = on; }
    bool BreakOnThreadCreate() const { return m_breakOnThreadCreate; }

    // Wait for the next debug event. Returns true while the session is alive;
    // false when the target exited or the wait failed. When it returns true,
    // call GetLastEvent() to see why we stopped.
    bool PumpOneEvent(DWORD timeoutMs = INFINITE);

    const BreakEvent& LastEvent() const { return m_last; }
    // Resume execution after a stop.
    void Go();
    // Interrupt a running target (Olly's Pause). The resulting stop shows up
    // as a plain Step event from PumpOneEvent (see its comment).
    void BreakIn();
    // Set a one-shot breakpoint at the current function's return address and
    // fill retAddr. Caller should Go() and watch for StepOutBpId(). Returns
    // false if the return address could not be read.
    bool StepOut(ULONG64& retAddr);
    ULONG StepOutBpId() const { return m_stepOutBpId; }
    void ClearStepOut();

    // Inspection while stopped.
    RegFile GetRegisters();
    ULONG64 GetRegister(const wchar_t* name);
    bool SetRegister(const wchar_t* name, ULONG64 value);
    bool ReadMemory(ULONG64 addr, void* buf, ULONG size, ULONG* got);
    bool WriteMemory(ULONG64 addr, const void* buf, ULONG size);
    bool Disasm(ULONG64 offset, DisasmLine& out);
    std::wstring SymbolAt(ULONG64 offset);
    std::wstring ReadAscii(ULONG64 addr, ULONG max = 128);
    std::wstring ReadWide(ULONG64 addr, ULONG max = 128);

    // Single-step. The completion shows up as StopReason::Step from the next
    // PumpOneEvent (stepping does not invoke any IDebugEventCallbacks method).
    void StepInto();
    void StepOver();

    // Remove a previously added breakpoint by id. Returns false if not found.
    bool RemoveBreakpointById(ULONG id);

    // Every breakpoint the engine currently holds, excluding the internal
    // step-out one (which is ours, not the user's, and must not be painted or
    // clearable in the disassembly).
    std::vector<BpInfo> Breakpoints();

    // Current process's PEB address (0 if unavailable). Plugins use this to
    // patch well-known anti-debug fields (BeingDebugged, NtGlobalFlag, ...).
    ULONG64 GetPeb();

    // SDK integration:
    // -map: load a text symbol map, one entry per line:
    //     <module> <rva> <name> [size]
    //   module names match the loaded DLL file name (case-insensitive); rva and
    //   size are hex (0x optional). These overlay the engine symbol names.
    bool LoadSymbolMap(const wchar_t* path);
    // -sdk: load an interface-method database, one entry per line:
    //     <interface> <slot> <method>
    //   (slot is 1-based vtable index). Used to name vtable slots.
    bool LoadSdk(const wchar_t* path);
    // Register a discovered vtable slot target: slot 0 == vtable base.
    // Names the target function <iface>::<method> in disassembly.
    void AddVtableTarget(ULONG64 vtableBase, const std::wstring& iface, int slot, const std::wstring& method);
    // Number of vtable slots known for an interface in the SDK db.
    int SdkSlotCount(const std::wstring& iface) const;
    // Interface method name at a 1-based slot, or empty.
    std::wstring SdkMethodAt(const std::wstring& iface, int slot) const;

private:
    friend class HostCallbacks;
    void SetStop(BreakEvent e) { m_last = e; }

    ComPtr<IDebugClient5>        m_client;
    ComPtr<IDebugControl4>       m_control;
    ComPtr<IDebugSymbols3>       m_symbols;
    ComPtr<IDebugSystemObjects4> m_sys;
    ComPtr<IDebugRegisters2>     m_regs;
    ComPtr<IDebugDataSpaces4>    m_data;
    class HostCallbacks*         m_cb = nullptr;
    BreakEvent                   m_last;
    bool                         m_verbose = false;
    std::wstring                 m_moduleBreak;
    ULONG                        m_stepOutBpId = DEBUG_ANY_ID;
    bool                         m_followChildren = false;
    std::wstring                 m_lastLaunch;       // for Restart()
    DWORD                        m_lastAttachPid = 0;
    // engineId -> executable name. Naming a process costs a current-process
    // switch, so each one is looked up once and remembered.
    std::vector<std::pair<ULONG, std::string>> m_procNames;
    bool                         m_breakOnModuleLoad = false;
    bool                         m_breakOnThreadCreate = false;
    // Seeded with the classic MS thread-name signalling exception (raised by
    // SetThreadName / RaiseException(0x406D1388) - pure noise to a debugger).
    std::vector<ExcRange>        m_ignoredExceptions = { { 0x406D1388, 0x406D1388 } };
    std::vector<ULONG>           m_seenExceptions;

    struct PendingBreak {
        std::wstring module;
        std::wstring symbol;
        bool         armed = false;
    };
    std::vector<PendingBreak>    m_pending;
    bool ResolveExportByBase(ULONG64 modBase, const std::wstring& symbol, ULONG64& out);
};
