#include "dbgsession.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>

namespace {

std::wstring A2W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &w[0], n);
    return w;
}

std::string W2A(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string a(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, &a[0], n, nullptr, nullptr);
    return a;
}

std::string Hex64(ULONG64 v) {
    char b[32];
    sprintf_s(b, "0x%llx", (unsigned long long)v);
    return b;
}

constexpr size_t kMaxLog = 500;
constexpr int kDisasmLines = 60;
constexpr ULONG kDumpBytes = 256;
constexpr int kStackEntries = 24;

} // namespace

DbgSession::DbgSession() {
    // DbgEng's interfaces are single-thread-affine: every call must come from
    // the thread that created them, or the engine silently stalls (no error,
    // no crash - just a hang on the next call). Init() therefore has to run
    // on m_worker itself, not here in the constructor (which runs on the
    // caller's/main thread).
    m_worker = std::thread([this] { WorkerMain(); });
    m_pipe = std::thread([this] { PipeMain(); });
}

DbgSession::~DbgSession() {
    m_quit = true;
    // Both threads may be blocked in a blocking OS wait (WaitForEvent with a
    // short timeout on the worker, ConnectNamedPipe/ReadFile on the pipe
    // thread). Detaching rather than joining avoids hanging process shutdown
    // on that wait; the OS reclaims both when the process exits.
    if (m_worker.joinable()) m_worker.detach();
    if (m_pipe.joinable()) m_pipe.detach();
}

void DbgSession::PushCommand(const std::wstring& text) {
    std::lock_guard<std::mutex> lk(m_qMutex);
    m_queue.push_back({ text, nullptr });
}

std::string DbgSession::PushCommandBlocking(const std::wstring& text) {
    std::promise<std::string> prom;
    auto fut = prom.get_future();
    {
        std::lock_guard<std::mutex> lk(m_qMutex);
        m_queue.push_back({ text, &prom });
    }
    return fut.get();
}

Snapshot DbgSession::GetSnapshot() {
    std::lock_guard<std::mutex> lk(m_stateMutex);
    return m_state;
}

void DbgSession::AppendLog_NoLock(const std::string& line) {
    m_state.log.push_back(line);
    if (m_state.log.size() > kMaxLog) m_state.log.erase(m_state.log.begin());
}

void DbgSession::SyncOptions_NoLock() {
    m_state.optChildDbg = m_host.FollowChildren();
    m_state.optBreakModule = m_host.BreakOnModuleLoad();
    m_state.optBreakThread = m_host.BreakOnThreadCreate();
    m_state.ignoredExceptions = m_host.IgnoredExceptions();
    m_state.seenExceptions = m_host.SeenExceptions();
    if (!m_host.LastLaunchCmdline().empty()) m_state.lastTarget = W2A(m_host.LastLaunchCmdline());

    // Breakpoint and process queries both need the engine in a break state
    // (enumerating breakpoints while the target free-runs is not safe, and
    // naming a process means briefly making it current). While running, the
    // GUI keeps painting whatever the last stop produced.
    if (m_state.sessionActive && m_state.stopped) {
        m_state.breakpoints = m_host.Breakpoints();
        SyncProcesses_NoLock();
    } else if (!m_state.sessionActive) {
        m_state.breakpoints.clear();
        for (auto& p : m_state.processes) p.alive = false;
        m_state.currentProcEngineId = 0;
    }
}

void DbgSession::SyncProcesses_NoLock() {
    std::vector<ProcInfo> live = m_host.Processes();
    for (auto& tab : m_state.processes) tab.alive = false;
    for (const auto& p : live) {
        ProcTab* tab = nullptr;
        for (auto& t : m_state.processes) if (t.engineId == p.engineId) { tab = &t; break; }
        if (!tab) {
            ProcTab t;
            t.engineId = p.engineId;
            m_state.processes.push_back(t);
            tab = &m_state.processes.back();
            // A process we have never seen before - the initial target, or a
            // child the debuggee just spawned. Pull the CPU window's tab to it.
            m_state.focusProcEngineId = p.engineId;
            m_state.procSerial++;
            AppendLog_NoLock("[proc] debugging " + (p.name.empty() ? std::string("process") : p.name) +
                             " (pid " + std::to_string(p.pid) + ")");
        }
        tab->alive = true;
        tab->pid = p.pid;
        if (!p.name.empty()) tab->name = p.name;
        if (tab->name.empty()) tab->name = "process " + std::to_string(p.pid);
    }
    m_state.currentProcEngineId = m_host.CurrentProcessEngineId();
}

void DbgSession::WorkerMain() {
    fprintf(stderr, "[worker] thread started\n"); fflush(stderr);
    m_workerThreadId = std::this_thread::get_id();
    m_host.SetVerbose(true);
    if (!m_host.Init()) {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        AppendLog_NoLock("! DbgEng init failed (dbgeng.dll missing?)");
        fprintf(stderr, "[worker] DbgHost::Init failed\n"); fflush(stderr);
        return;
    }
    fprintf(stderr, "[worker] DbgHost::Init ok\n"); fflush(stderr);

    wchar_t exeDirBuf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeDirBuf, MAX_PATH);
    std::wstring exeDir(exeDirBuf);
    {
        size_t slash = exeDir.find_last_of(L"\\/");
        exeDir = slash == std::wstring::npos ? L"." : exeDir.substr(0, slash);
    }

    // On-demand symbol server: symsrv.dll (already vendored alongside
    // dbgeng.dll) transparently downloads and caches PDBs the first time a
    // module's symbols are actually needed (disasm headers, stack/register
    // symbol names, ...) - no per-launch -sym flag required.
    std::wstring symCache = exeDir + L"\\symcache";
    m_host.SetSymbolPath(L"srv*" + symCache + L"*https://msdl.microsoft.com/download/symbols");
    m_host.ReloadSymbols();

    {
        std::wstring dir = exeDir + L"\\plugins";
        m_plugins.LoadFromDirectory(dir);
        std::lock_guard<std::mutex> lk(m_stateMutex);
        for (size_t i = 0; i < m_plugins.Count(); i++)
            AppendLog_NoLock("[plugin] loaded: " + m_plugins.Name(i));
        SyncOptions_NoLock();
    }

    for (;;) {
        if (m_quit) return;

        for (;;) {
            QueuedCmd cmd;
            {
                std::lock_guard<std::mutex> lk(m_qMutex);
                if (m_queue.empty()) break;
                cmd = std::move(m_queue.front());
                m_queue.pop_front();
            }
            fprintf(stderr, "[worker] dequeued: %ls\n", cmd.text.c_str()); fflush(stderr);
            HandleCommand(cmd);
            fprintf(stderr, "[worker] handled: %ls\n", cmd.text.c_str()); fflush(stderr);
        }
        if (m_quit) return;

        {
            std::function<void()> job;
            for (;;) {
                {
                    std::lock_guard<std::mutex> lk(m_jobMutex);
                    if (m_jobs.empty()) break;
                    job = std::move(m_jobs.front());
                    m_jobs.pop_front();
                }
                job();
            }
        }
        if (m_quit) return;

        bool active, stopped;
        {
            std::lock_guard<std::mutex> lk(m_stateMutex);
            active = m_state.sessionActive;
            stopped = m_state.stopped;
        }

        if (active && !stopped) {
            if (m_host.PumpOneEvent(50)) {
                fprintf(stderr, "[worker] event reason=%d\n", (int)m_host.LastEvent().reason); fflush(stderr);
                HandleEvent(m_host.LastEvent());
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void DbgSession::HandleCommand(const QueuedCmd& cmd) {
    bool active, stopped;
    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        active = m_state.sessionActive;
        stopped = m_state.stopped;
    }

    // Editing breakpoints requires the engine to be in a break state (AddBreakpoint
    // returns E_UNEXPECTED otherwise). If the target is free-running, interrupt it,
    // make the edit, then resume - so bp/bc feel seamless while running, like Olly.
    std::wstring verb = cmd.text.substr(0, cmd.text.find(L' '));
    std::transform(verb.begin(), verb.end(), verb.begin(), ::towlower);

    // `proc` / `closeproc` act on the CPU window's tab list, which lives here
    // rather than in DbgHost, so they never reach the shared dispatcher.
    if (verb == L"proc" || verb == L"closeproc") {
        size_t sp = cmd.text.find(L' ');
        ULONG engineId = sp == std::wstring::npos ? 0 : (ULONG)_wtoi(cmd.text.c_str() + sp + 1);
        std::string out;
        {
            std::lock_guard<std::mutex> lk(m_stateMutex);
            AppendLog_NoLock("> " + W2A(cmd.text));
            if (verb == L"closeproc") {
                bool erased = false;
                for (size_t i = 0; i < m_state.processes.size(); i++) {
                    if (m_state.processes[i].engineId != engineId) continue;
                    // Only a dead tab is closable - closing a live one would
                    // hide a process that is still being debugged.
                    if (m_state.processes[i].alive) { out = "process is still alive"; break; }
                    m_state.processes.erase(m_state.processes.begin() + i);
                    erased = true;
                    break;
                }
                if (erased) out = "closed process tab";
                else if (out.empty()) out = "no such process tab";
            } else if (!m_state.sessionActive || !m_state.stopped) {
                out = "can only switch process while stopped";
            } else if (m_host.SetCurrentProcess(engineId)) {
                m_state.currentProcEngineId = engineId;
                RefreshAfterStop_NoLock();
                m_state.breakpoints = m_host.Breakpoints();
                out = "switched to process " + std::to_string(engineId);
            } else {
                out = "no such process";
            }
            AppendLog_NoLock("  " + out);
        }
        if (cmd.promise) cmd.promise->set_value(out);
        return;
    }

    bool needsBreakToEdit = (verb == L"bp" || verb == L"bpx" || verb == L"bc");
    bool weInterrupted = false;

    if (active && needsBreakToEdit && !stopped) {
        m_host.BreakIn();
        // A single generous wait, not a string of short polls: SetInterrupt
        // queues a request the engine services on its own schedule, and
        // resetting the wait every 50ms seemed to starve it of the chance.
        if (m_host.PumpOneEvent(5000)) {
            fprintf(stderr, "[interrupt-wait] event reason=%d\n", (int)m_host.LastEvent().reason); fflush(stderr);
            HandleEvent(m_host.LastEvent());
        }
        std::lock_guard<std::mutex> lk(m_stateMutex);
        stopped = m_state.stopped;
        fprintf(stderr, "[interrupt-wait] done, stopped=%d\n", (int)stopped); fflush(stderr);
        weInterrupted = stopped;
    }

    DispatchResult res = DispatchCommand(m_host, cmd.text, active, stopped);

    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        AppendLog_NoLock("> " + W2A(cmd.text));
        AppendLog_NoLock((res.ok ? "  " : "  ! ") + res.output);

        if (res.sessionStarted) {
            m_state.sessionActive = true;
            m_state.stopped = false;
            // Tabs and breakpoints belong to the session that just ended (this
            // is a fresh launch, or a restart) - start the CPU window clean.
            m_state.processes.clear();
            m_state.breakpoints.clear();
            m_state.currentProcEngineId = 0;
        }
        if (res.sessionEnded) {
            m_state.sessionActive = false;
            m_state.stopped = false;
            // Keep the tabs: they carry the X marker that says the process died.
            for (auto& p : m_state.processes) p.alive = false;
        }
        if (res.resumed) {
            m_state.stopped = false;
        }
        if (res.viewKind == ViewKind::Disasm) {
            m_state.disasmViewAddr = res.viewAddr;
            RefreshDisasmView_NoLock();
        } else if (res.viewKind == ViewKind::Dump) {
            m_state.dumpViewAddr = res.viewAddr;
            m_state.dumpWidth = res.dumpWidth;
            RefreshDumpView_NoLock();
        }

        if (weInterrupted) {
            m_host.Go();
            m_state.stopped = false;
            AppendLog_NoLock("(resumed after edit)");
        }
        // A command may have flipped an option (childdbg/breakmod/ignoreexc/...);
        // refresh the mirrored option state the GUI reads.
        SyncOptions_NoLock();
    }

    if (cmd.promise) cmd.promise->set_value(res.output);
}

void DbgSession::HandleEvent(const BreakEvent& ev) {
    // Plugins are fired AFTER the lock is dropped: a plugin's Odbg_Paused can
    // call Odbg_Log / Odbg_* which re-acquire m_stateMutex (a non-recursive
    // mutex), so firing under the lock deadlocks/crashes.
    int fireReason = -1;
    OdbgRegs pluginRegs{};
    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        switch (ev.reason) {
        case StopReason::ModuleLoaded:
            // Only fires when a module-break option is on (break-on-all or the
            // name filter), so honour it as a real pause, Olly-style.
            AppendLog_NoLock("[module] " + W2A(ev.module) + " @ " + Hex64(ev.moduleBase) + " - paused");
            m_state.stopped = true;
            pluginRegs = RefreshAfterStop_NoLock();
            fireReason = ODBG_PAUSE_ATTACH_OR_LAUNCH;
            break;
        case StopReason::ThreadCreated:
            AppendLog_NoLock("[thread] new thread, start @ " + Hex64(ev.offset) + " - paused");
            m_state.stopped = true;
            pluginRegs = RefreshAfterStop_NoLock();
            fireReason = ODBG_PAUSE_ATTACH_OR_LAUNCH;
            break;
        case StopReason::Exception: {
            // Olly's default is to break on every exception rather than pass it
            // silently to the debuggee - do the same here. This also means a
            // freshly launched target stops at the initial system breakpoint
            // instead of auto-continuing straight into its own code, which
            // matters for targets that do something (e.g. network login) very
            // early that you want a chance to intercept first.
            char b[64];
            sprintf_s(b, "[exception] code=0x%08x", ev.exceptionCode);
            AppendLog_NoLock(b);
            m_state.stopped = true;
            pluginRegs = RefreshAfterStop_NoLock();
            fireReason = ODBG_PAUSE_ATTACH_OR_LAUNCH;
            break;
        }
        case StopReason::ProcessExited: {
            // The exiting process is the current one for the duration of this
            // event - the only moment we can name it. Flag its tab dead now, or
            // it would keep its live marker until the next stop (the process
            // list can only be walked in a break state).
            ULONG dying = m_host.CurrentProcessEngineId();
            for (auto& p : m_state.processes) if (p.engineId == dying) p.alive = false;

            // When following children, keep the session alive if other debuggees
            // remain (a launcher exits after handing off to the real process).
            ULONG remaining = m_host.FollowChildren() ? m_host.NumProcesses() : 0;
            if (remaining > 1) {
                AppendLog_NoLock("[exit] one process exited, " + std::to_string(remaining - 1) + " still debugged");
                m_host.Go();
                break;
            }
            AppendLog_NoLock("[exit] process exited");
            m_host.EndSession();
            m_state.sessionActive = false;
            m_state.stopped = false;
            break;
        }
        case StopReason::Breakpoint:
            AppendLog_NoLock("[bp " + std::to_string(ev.bpId) + "] " + W2A(m_host.SymbolAt(ev.offset)));
            m_state.stopped = true;
            pluginRegs = RefreshAfterStop_NoLock();
            fireReason = ODBG_PAUSE_BREAKPOINT;
            break;
        case StopReason::Step:
            m_state.stopped = true;
            pluginRegs = RefreshAfterStop_NoLock();
            fireReason = ODBG_PAUSE_STEP;
            break;
        default:
            m_host.Go();
            break;
        }
        SyncOptions_NoLock();  // refresh seen-exceptions/option mirror for the GUI
    }
    if (fireReason >= 0) m_plugins.FirePaused(fireReason, pluginRegs);
}

OdbgRegs DbgSession::RefreshAfterStop_NoLock() {
    m_state.regs = m_host.GetRegisters();
    m_state.disasmViewAddr = m_state.regs.rip;
    RefreshDisasmView_NoLock();
    if (m_state.dumpViewAddr == 0) m_state.dumpViewAddr = m_state.regs.rsp;
    RefreshDumpView_NoLock();
    RefreshStackView_NoLock();

    OdbgRegs r{};
    r.rax = m_state.regs.rax; r.rbx = m_state.regs.rbx; r.rcx = m_state.regs.rcx; r.rdx = m_state.regs.rdx;
    r.rsi = m_state.regs.rsi; r.rdi = m_state.regs.rdi; r.rbp = m_state.regs.rbp; r.rsp = m_state.regs.rsp;
    r.rip = m_state.regs.rip;
    r.r8 = m_state.regs.r8; r.r9 = m_state.regs.r9; r.r10 = m_state.regs.r10; r.r11 = m_state.regs.r11;
    return r;
}

void DbgSession::RefreshDisasmView_NoLock() {
    m_state.disasmLines.clear();
    ULONG64 addr = m_state.disasmViewAddr;
    for (int i = 0; i < kDisasmLines; i++) {
        DisasmLine dl;
        if (!m_host.Disasm(addr, dl)) break;
        m_state.disasmLines.push_back(dl);
        if (!dl.next || dl.next == dl.addr) break;
        addr = dl.next;
    }
}

void DbgSession::RefreshDumpView_NoLock() {
    m_state.dumpBytes.assign(kDumpBytes, 0);
    ULONG got = 0;
    if (!m_host.ReadMemory(m_state.dumpViewAddr, m_state.dumpBytes.data(), kDumpBytes, &got))
        got = 0;
    m_state.dumpBytes.resize(got);
}

void DbgSession::RefreshStackView_NoLock() {
    m_state.stack.clear();
    ULONG64 addr = m_state.regs.rsp;
    for (int i = 0; i < kStackEntries; i++) {
        ULONG64 v = 0;
        ULONG got = 0;
        if (!m_host.ReadMemory(addr, &v, sizeof(v), &got) || got != sizeof(v)) break;
        StackEntry e;
        e.addr = addr;
        e.value = v;
        e.symbol = m_host.SymbolAt(v);
        m_state.stack.push_back(e);
        addr += sizeof(ULONG64);
    }
}

bool DbgSession::SafeReadMemory(ULONG64 addr, void* buf, ULONG size) {
    ULONG got = 0;
    return RunOnWorker([&] { return m_host.ReadMemory(addr, buf, size, &got); });
}

bool DbgSession::SafeWriteMemory(ULONG64 addr, const void* buf, ULONG size) {
    return RunOnWorker([&] { return m_host.WriteMemory(addr, buf, size); });
}

ULONG64 DbgSession::SafeGetRegister(const wchar_t* name) {
    return RunOnWorker([&] { return m_host.GetRegister(name); });
}

bool DbgSession::SafeSetRegister(const wchar_t* name, ULONG64 value) {
    return RunOnWorker([&] { return m_host.SetRegister(name, value); });
}

RegFile DbgSession::SafeGetRegisters() {
    return RunOnWorker([&] { return m_host.GetRegisters(); });
}

int DbgSession::SafeAddBreakpoint(const std::wstring& expr) {
    return RunOnWorker([&] {
        return expr.find(L'!') != std::wstring::npos
            ? m_host.AddDeferredBreakpoint(expr)
            : m_host.AddOffsetBreakpoint(_wcstoui64(expr.c_str(), nullptr, 16));
    });
}

bool DbgSession::SafeRemoveBreakpoint(int id) {
    return RunOnWorker([&] { return m_host.RemoveBreakpointById((ULONG)id); });
}

ULONG64 DbgSession::SafeGetPeb() {
    return RunOnWorker([&] { return m_host.GetPeb(); });
}

void DbgSession::SafeGo() {
    RunOnWorker([&] {
        m_host.Go();
        std::lock_guard<std::mutex> lk(m_stateMutex);
        m_state.stopped = false;
    });
}
void DbgSession::SafePause() { RunOnWorker([&] { m_host.BreakIn(); }); }
void DbgSession::SafeStepInto() {
    RunOnWorker([&] {
        m_host.StepInto();
        std::lock_guard<std::mutex> lk(m_stateMutex);
        m_state.stopped = false;
    });
}
void DbgSession::SafeStepOver() {
    RunOnWorker([&] {
        m_host.StepOver();
        std::lock_guard<std::mutex> lk(m_stateMutex);
        m_state.stopped = false;
    });
}

bool DbgSession::IsSessionActive() {
    std::lock_guard<std::mutex> lk(m_stateMutex);
    return m_state.sessionActive;
}

bool DbgSession::IsStopped() {
    std::lock_guard<std::mutex> lk(m_stateMutex);
    return m_state.stopped;
}

void DbgSession::LogFromPlugin(const std::string& line) {
    std::lock_guard<std::mutex> lk(m_stateMutex);
    AppendLog_NoLock(line);
}

void DbgSession::PipeMain() {
    const wchar_t* pipeName = L"\\\\.\\pipe\\odbg_cmd";
    for (;;) {
        if (m_quit) return;
        HANDLE h = CreateNamedPipeW(pipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 8192, 8192, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;

        BOOL connected = ::ConnectNamedPipe(h, nullptr) ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected && !m_quit) {
            // Strictly one request per connection: read one message, reply,
            // disconnect. Not looping to read a second message means we never
            // sit blocked on a client that has already gone away instead of
            // going back to accept the next one.
            char buf[4096];
            DWORD read = 0;
            if (::ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
                buf[read] = 0;
                std::string line(buf);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

                std::string result = PushCommandBlocking(A2W(line));

                DWORD written = 0;
                ::WriteFile(h, result.c_str(), (DWORD)result.size(), &written, nullptr);
                ::FlushFileBuffers(h);
            }
        }
        ::DisconnectNamedPipe(h);
        ::CloseHandle(h);
    }
}
