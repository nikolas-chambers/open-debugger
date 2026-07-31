#include "dbghost.h"

#include <algorithm>
#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// Event callbacks: the engine hands us every debug event here.  We translate
// what we care about into a StopReason and remember it in the host; the main
// loop wakes up, inspects, and calls Go().
// ---------------------------------------------------------------------------

class HostCallbacks : public IDebugEventCallbacks {
public:
    HostCallbacks(DbgHost& host) : m_host(host) {}

    // IUnknown
    STDMETHOD(QueryInterface)(THIS_ IN REFIID iid, OUT void** ppv) override {
        if (iid == IID_IUnknown) { *ppv = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
        if (iid == __uuidof(IDebugEventCallbacks)) { *ppv = static_cast<IDebugEventCallbacks*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)(THIS) override { return InterlockedIncrement(&m_refs); }
    STDMETHOD_(ULONG, Release)(THIS) override {
        ULONG r = InterlockedDecrement(&m_refs);
        if (r == 0) delete this;
        return r;
    }

    // IDebugEventCallbacks (we only implement the events we use; the rest
    // return DEBUG_STATUS_NO_CHANGE so the debugger keeps going).
    STDMETHOD(GetInterestMask)(THIS_ OUT ULONG* Mask) override {
        *Mask = DEBUG_EVENT_BREAKPOINT | DEBUG_EVENT_EXCEPTION |
                DEBUG_EVENT_CREATE_PROCESS | DEBUG_EVENT_EXIT_PROCESS |
                DEBUG_EVENT_LOAD_MODULE | DEBUG_EVENT_UNLOAD_MODULE |
                DEBUG_EVENT_CREATE_THREAD | DEBUG_EVENT_EXIT_THREAD |
                DEBUG_EVENT_SESSION_STATUS | DEBUG_EVENT_SYSTEM_ERROR;
        return S_OK;
    }

    STDMETHOD(Breakpoint)(THIS_ IN PDEBUG_BREAKPOINT Bp) override {
        ULONG id = 0;
        Bp->GetId(&id);
        ULONG64 off = 0;
        Bp->GetOffset(&off);
        if (m_host.m_verbose) wprintf(L"[cb] Breakpoint id=%u off=0x%llx -> BREAK\n", id, (unsigned long long)off);
        BreakEvent e;
        e.reason = StopReason::Breakpoint;
        e.bpId = id;
        e.offset = off;
        m_host.SetStop(e);
        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(Exception)(THIS_ IN PEXCEPTION_RECORD64 Exception, IN ULONG FirstChance) override {
        // Remember every code that fires so the Exceptions page can show what
        // this target actually throws (one-click to add it to the ignore list).
        m_host.RecordSeenException(Exception->ExceptionCode);
        // OllyDbg-style ignored exceptions: pass the code straight to the
        // debuggee's own handler and keep running, without ever surfacing a
        // stop to the front-end. GO_NOT_HANDLED means "I didn't handle it,
        // let the app's SEH chain have it" - exactly how a non-debugged
        // process would see it.
        if (m_host.IsExceptionIgnored(Exception->ExceptionCode)) {
            if (m_host.m_verbose) wprintf(L"[cb] Exception code=0x%08x addr=0x%llx (ignored, passed to debuggee)\n",
                Exception->ExceptionCode, (unsigned long long)Exception->ExceptionAddress);
            return DEBUG_STATUS_GO_NOT_HANDLED;
        }
        if (m_host.m_verbose) wprintf(L"[cb] Exception code=0x%08x addr=0x%llx firstChance=%u\n",
            Exception->ExceptionCode, (unsigned long long)Exception->ExceptionAddress, FirstChance);
        BreakEvent e;
        e.reason = StopReason::Exception;
        e.exceptionCode = Exception->ExceptionCode;
        e.offset = Exception->ExceptionAddress;
        m_host.SetStop(e);
        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(ExitProcess)(THIS_ IN ULONG ExitCode) override {
        if (m_host.m_verbose) wprintf(L"[cb] ExitProcess code=%u -> BREAK\n", ExitCode);
        BreakEvent e;
        e.reason = StopReason::ProcessExited;
        m_host.SetStop(e);
        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(LoadModule)(THIS_ IN ULONG64 ImageFileHandle,
                          IN ULONG64 BaseOffset,
                          IN ULONG ModuleSize,
                          IN PCSTR ModuleName,
                          IN PCSTR ImageName,
                          IN ULONG CheckSum,
                          IN ULONG TimeDateStamp) override {
        std::wstring name = NarrowToWide(ModuleName);
        m_host.ResolvePendingForModule(name, BaseOffset);
        bool matchName = false;
        if (!m_host.m_moduleBreak.empty()) {
            std::wstring lower = name;
            std::wstring want = m_host.m_moduleBreak;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            std::transform(want.begin(), want.end(), want.begin(), ::towlower);
            matchName = lower.find(want) != std::wstring::npos;
        }
        // Break-on-module-load (all DLLs) OR the substring filter matched.
        if (m_host.m_breakOnModuleLoad || matchName) {
            BreakEvent e;
            e.reason = StopReason::ModuleLoaded;
            e.module = name;
            e.moduleBase = BaseOffset;
            m_host.SetStop(e);
            return DEBUG_STATUS_BREAK;
        }
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(CreateProcess)(THIS_ IN ULONG64 ImageFileHandle,
                             IN ULONG64 Handle,
                             IN ULONG64 BaseOffset,
                             IN ULONG ModuleSize,
                             IN PCSTR ModuleName,
                             IN PCSTR ImageName,
                             IN ULONG CheckSum,
                             IN ULONG TimeDateStamp,
                             IN ULONG64 InitialThreadHandle,
                             IN ULONG64 ThreadDataOffset,
                             IN ULONG64 StartOffset) override {
        // Fires for the initial process and, when child-following is on, for
        // every spawned child. Don't stop here (the child stops at its own
        // loader int3, reported via Exception); just note it so the trace shows
        // a child was picked up.
        if (m_host.m_verbose)
            wprintf(L"[cb] CreateProcess module=%hs base=0x%llx\n",
                ModuleName ? ModuleName : "?", (unsigned long long)BaseOffset);
        return DEBUG_STATUS_NO_CHANGE;
    }
    STDMETHOD(UnloadModule)(THIS_ IN PCSTR, IN ULONG64) override { return DEBUG_STATUS_NO_CHANGE; }
    STDMETHOD(ExitThread)(THIS_ IN ULONG ExitCode) override { return DEBUG_STATUS_NO_CHANGE; }
    STDMETHOD(CreateThread)(THIS_ IN ULONG64 Handle, IN ULONG64 DataOffset, IN ULONG64 StartOffset) override {
        if (m_host.m_breakOnThreadCreate) {
            if (m_host.m_verbose) wprintf(L"[cb] CreateThread start=0x%llx -> BREAK\n", (unsigned long long)StartOffset);
            BreakEvent e;
            e.reason = StopReason::ThreadCreated;
            e.offset = StartOffset;
            m_host.SetStop(e);
            return DEBUG_STATUS_BREAK;
        }
        return DEBUG_STATUS_NO_CHANGE;
    }
    STDMETHOD(SystemError)(THIS_ IN ULONG Error, IN ULONG Level) override {
        if (m_host.m_verbose) wprintf(L"[cb] SystemError error=%u level=%u\n", Error, Level);
        return DEBUG_STATUS_NO_CHANGE;
    }
    STDMETHOD(SessionStatus)(THIS_ IN ULONG Status) override {
        if (m_host.m_verbose) wprintf(L"[cb] SessionStatus 0x%08x\n", Status);
        return DEBUG_STATUS_NO_CHANGE;
    }
    STDMETHOD(ChangeDebuggeeState)(THIS_ IN ULONG Flags, IN ULONG64 Argument) override { return DEBUG_STATUS_NO_CHANGE; }
    STDMETHOD(ChangeEngineState)(THIS_ IN ULONG Flags, IN ULONG64 Argument) override { return DEBUG_STATUS_NO_CHANGE; }
    STDMETHOD(ChangeSymbolState)(THIS_ IN ULONG Flags, IN ULONG64 Argument) override { return DEBUG_STATUS_NO_CHANGE; }

private:
    static std::wstring NarrowToWide(PCSTR s) {
        if (!s) return L"";
        int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n) MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
        return w;
    }
    DbgHost& m_host;
    LONG m_refs = 1;
};

// ---------------------------------------------------------------------------

static std::wstring A2W(PCSTR s) {
    if (!s) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}

static std::string W2A(PCWSTR s) {
    if (!s) return "";
    int n = WideCharToMultiByte(CP_ACP, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string a(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_ACP, 0, s, -1, &a[0], n, nullptr, nullptr);
    return a;
}

DbgHost::DbgHost() {}

DbgHost::~DbgHost() { Shutdown(); }

bool DbgHost::Init() {
    HRESULT hr = DebugCreate(__uuidof(IDebugClient5), (void**)&m_client);
    if (FAILED(hr)) return false;
    if (FAILED(m_client->QueryInterface(__uuidof(IDebugControl4), (void**)&m_control))) return false;
    if (FAILED(m_client->QueryInterface(__uuidof(IDebugSymbols3), (void**)&m_symbols))) return false;
    if (FAILED(m_client->QueryInterface(__uuidof(IDebugSystemObjects4), (void**)&m_sys))) return false;
    if (FAILED(m_client->QueryInterface(__uuidof(IDebugRegisters2), (void**)&m_regs))) return false;
    if (FAILED(m_client->QueryInterface(__uuidof(IDebugDataSpaces4), (void**)&m_data))) return false;
    if (!m_control || !m_symbols || !m_sys || !m_regs || !m_data) return false;

    m_cb = new HostCallbacks(*this);
    m_client->SetEventCallbacks(m_cb);

    // Stop at the initial loader breakpoint; without this the target is
    // auto-continued at launch/attach and WaitForEvent never sees a stop.
    ULONG opts = 0;
    m_control->GetEngineOptions(&opts);
    opts |= DEBUG_ENGOPT_INITIAL_BREAK;
    m_control->SetEngineOptions(opts);
    return true;
}

void DbgHost::Shutdown() {
    if (m_cb) {
        m_client->SetEventCallbacks(nullptr);
        m_cb->Release();
        m_cb = nullptr;
    }
    m_client.reset();
    m_control.reset();
    m_symbols.reset();
    m_sys.reset();
    m_regs.reset();
    m_data.reset();
}

void DbgHost::SetSymbolPath(const std::wstring& path) {
    m_symbols->SetSymbolPathWide(path.c_str());
}

void DbgHost::ReloadSymbols() {
    m_symbols->Reload("");
}

bool DbgHost::Launch(const std::wstring& cmdline) {
    std::string a = W2A(cmdline.c_str());
    HRESULT hr = m_client->CreateProcessAndAttach(
        0, const_cast<char*>(a.c_str()), DEBUG_PROCESS, 0, 0);
    if (FAILED(hr) && m_verbose) wprintf(L"[dbg] CreateProcessAndAttach failed hr=0x%08x\n", hr);
    // Re-assert the child-follow setting now the engine definitely has a target
    // (covers callers that flipped the option before a session existed).
    if (SUCCEEDED(hr)) {
        m_control->Execute(DEBUG_OUTCTL_IGNORE, m_followChildren ? ".childdbg 1" : ".childdbg 0",
                           DEBUG_EXECUTE_NOT_LOGGED);
        m_lastLaunch = cmdline;
        m_lastAttachPid = 0;
        m_procNames.clear();
    }
    return SUCCEEDED(hr);
}

bool DbgHost::Attach(DWORD pid) {
    HRESULT hr = m_client->AttachProcess(0, pid, DEBUG_ATTACH_DEFAULT);
    if (FAILED(hr)) wprintf(L"[dbg] AttachProcess hr=0x%08x\n", hr);
    if (SUCCEEDED(hr)) {
        m_lastAttachPid = pid;
        m_lastLaunch.clear();
        m_procNames.clear();
    }
    return SUCCEEDED(hr);
}

bool DbgHost::Restart() {
    // Nothing to repeat: this host was never given a target.
    if (m_lastLaunch.empty() && !m_lastAttachPid) return false;
    // Copy first - EndSession() does not clear these, but Launch/Attach below
    // rewrite them, and Launch() reads its argument by reference.
    std::wstring cmdline = m_lastLaunch;
    DWORD pid = m_lastAttachPid;
    EndSession();
    ClearStepOut();
    m_pending.clear();
    m_procNames.clear();
    return cmdline.empty() ? Attach(pid) : Launch(cmdline);
}

void DbgHost::SetFollowChildren(bool on) {
    m_followChildren = on;
    // .childdbg is a live engine setting; apply now if the engine is up so the
    // toggle takes effect for children spawned from this point on.
    if (m_control)
        m_control->Execute(DEBUG_OUTCTL_IGNORE, on ? ".childdbg 1" : ".childdbg 0",
                           DEBUG_EXECUTE_NOT_LOGGED);
}

ULONG DbgHost::NumProcesses() {
    ULONG n = 0;
    if (m_sys && SUCCEEDED(m_sys->GetNumberProcesses(&n))) return n;
    return 0;
}

ULONG DbgHost::CurrentProcessEngineId() {
    ULONG id = 0;
    if (m_sys && SUCCEEDED(m_sys->GetCurrentProcessId(&id))) return id;
    return 0;
}

bool DbgHost::SetCurrentProcess(ULONG engineId) {
    if (!m_sys) return false;
    return SUCCEEDED(m_sys->SetCurrentProcessId(engineId));
}

std::vector<ProcInfo> DbgHost::Processes() {
    std::vector<ProcInfo> out;
    if (!m_sys) return out;
    ULONG n = 0;
    if (FAILED(m_sys->GetNumberProcesses(&n)) || !n) return out;

    std::vector<ULONG> engineIds(n), sysIds(n);
    if (FAILED(m_sys->GetProcessIdsByIndex(0, n, engineIds.data(), sysIds.data()))) return out;

    ULONG cur = CurrentProcessEngineId();
    for (ULONG i = 0; i < n; i++) {
        ProcInfo p;
        p.engineId = engineIds[i];
        p.pid = sysIds[i];

        bool known = false;
        for (const auto& kv : m_procNames)
            if (kv.first == p.engineId) { p.name = kv.second; known = true; break; }
        // Only pay the current-process switch the first time we see a process;
        // after that the name is cached for the life of the session.
        if (!known && SUCCEEDED(m_sys->SetCurrentProcessId(p.engineId))) {
            char buf[MAX_PATH] = "";
            ULONG got = 0;
            if (SUCCEEDED(m_sys->GetCurrentProcessExecutableName(buf, sizeof(buf) - 1, &got)) && buf[0]) {
                std::string full(buf);
                size_t slash = full.find_last_of("\\/");
                p.name = slash == std::string::npos ? full : full.substr(slash + 1);
            }
            if (p.name.empty()) p.name = "process " + std::to_string(p.pid);
            m_procNames.emplace_back(p.engineId, p.name);
        }
        out.push_back(p);
    }
    if (cur) m_sys->SetCurrentProcessId(cur);
    return out;
}

void DbgHost::AddIgnoredExceptionRange(ULONG lo, ULONG hi) {
    if (hi < lo) std::swap(lo, hi);
    for (const auto& r : m_ignoredExceptions) if (r.lo == lo && r.hi == hi) return;
    m_ignoredExceptions.push_back({ lo, hi });
}

void DbgHost::RemoveIgnoredExceptionAt(size_t index) {
    if (index < m_ignoredExceptions.size())
        m_ignoredExceptions.erase(m_ignoredExceptions.begin() + index);
}

void DbgHost::RemoveIgnoredException(ULONG code) {
    m_ignoredExceptions.erase(
        std::remove_if(m_ignoredExceptions.begin(), m_ignoredExceptions.end(),
            [code](const ExcRange& r) { return r.lo == code && r.hi == code; }),
        m_ignoredExceptions.end());
}

bool DbgHost::IsExceptionIgnored(ULONG code) const {
    for (const auto& r : m_ignoredExceptions) if (code >= r.lo && code <= r.hi) return true;
    return false;
}

void DbgHost::RecordSeenException(ULONG code) {
    for (ULONG c : m_seenExceptions) if (c == code) return;
    m_seenExceptions.push_back(code);
}

void DbgHost::EndSession() {
    HRESULT hr = m_client->EndSession(DEBUG_END_ACTIVE_TERMINATE);
    if (m_verbose) wprintf(L"[dbg] EndSession hr=0x%08x\n", hr);
}

int DbgHost::AddDeferredBreakpoint(const std::wstring& expr) {
    // Parse "module!symbol".
    size_t bang = expr.find(L'!');
    std::wstring module = bang == std::wstring::npos ? L"" : expr.substr(0, bang);
    std::wstring symbol = bang == std::wstring::npos ? expr : expr.substr(bang + 1);

    ComPtr<IDebugBreakpoint> bp;
    HRESULT hr = m_control->AddBreakpoint(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, &bp);
    if (FAILED(hr)) {
        if (m_verbose) wprintf(L"[dbg] AddBreakpoint failed hr=0x%08x\n", hr);
        return -1;
    }

    ULONG64 off = 0;
    if (!module.empty() && ResolveExport(module, symbol, off)) {
        hr = bp->SetOffset(off);
        if (FAILED(hr)) return -1;
        bp->AddFlags(DEBUG_BREAKPOINT_ENABLED);
        ULONG id = 0;
        bp->GetId(&id);
        if (m_verbose) wprintf(L"[dbg] bp %u <- %s (resolved 0x%llx)\n", id, expr.c_str(), (unsigned long long)off);
        return (int)id;
    }

    // Not resolvable yet (module not loaded): keep as a deferred expression AND
    // queue it for export-table arming on module load.
    hr = bp->SetOffsetExpression(W2A(expr.c_str()).c_str());
    if (FAILED(hr)) {
        if (m_verbose) wprintf(L"[dbg] breakpoint '%s' deferred set failed hr=0x%08x\n", expr.c_str(), hr);
        return -1;
    }
    bp->AddFlags(DEBUG_BREAKPOINT_ENABLED);
    ULONG id = 0;
    bp->GetId(&id);
    if (m_verbose) wprintf(L"[dbg] bp %u <- %s (deferred)\n", id, expr.c_str());
    if (!module.empty()) {
        bool exists = false;
        for (auto& p : m_pending) if (p.module == module && p.symbol == symbol) { exists = true; break; }
        if (!exists) m_pending.push_back({ module, symbol, false });
    }
    return (int)id;
}

bool DbgHost::ResolveExportByBase(ULONG64 modBase, const std::wstring& symbol, ULONG64& out) {
    if (!m_data || !modBase) return false;
    IMAGE_DOS_HEADER dos = {};
    ULONG got = 0;
    if (FAILED(m_data->ReadVirtual(modBase, &dos, sizeof(dos), &got)) || got != sizeof(dos))
        return false;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS64 nt = {};
    if (FAILED(m_data->ReadVirtual(modBase + dos.e_lfanew, &nt, sizeof(nt), &got)) || got != sizeof(nt))
        return false;
    if (nt.Signature != IMAGE_NT_SIGNATURE) return false;
    ULONG edRva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (!edRva) return false;
    IMAGE_EXPORT_DIRECTORY ed = {};
    if (FAILED(m_data->ReadVirtual(modBase + edRva, &ed, sizeof(ed), &got)) || got != sizeof(ed))
        return false;
    if (!ed.NumberOfNames) return false;

    std::vector<ULONG> nameRvas(ed.NumberOfNames);
    std::vector<USHORT> ords(ed.NumberOfNames);
    std::vector<ULONG> funcRvas(ed.NumberOfFunctions);
    ULONG n = ed.NumberOfNames;
    if (FAILED(m_data->ReadVirtual(modBase + ed.AddressOfNames, nameRvas.data(), n * 4, &got)) || got != n * 4) return false;
    if (FAILED(m_data->ReadVirtual(modBase + ed.AddressOfNameOrdinals, ords.data(), n * 2, &got)) || got != n * 2) return false;
    if (FAILED(m_data->ReadVirtual(modBase + ed.AddressOfFunctions, funcRvas.data(), ed.NumberOfFunctions * 4, &got)) || got != ed.NumberOfFunctions * 4) return false;

    for (ULONG i = 0; i < n; i++) {
        char nm[256] = "";
        if (FAILED(m_data->ReadVirtual(modBase + nameRvas[i], nm, 255, &got))) continue;
        nm[255] = 0;
        std::wstring ws = A2W(nm);
        if (_wcsicmp(ws.c_str(), symbol.c_str()) != 0) continue;
        ULONG ord = ords[i];
        if (ord >= ed.NumberOfFunctions) continue;
        ULONG fnRva = funcRvas[ord];
        // Forwarders point back into the export directory region.
        if (fnRva >= edRva && fnRva < edRva + sizeof(ed) + ed.NumberOfNames * 4) continue;
        out = modBase + fnRva;
        return true;
    }
    return false;
}

bool DbgHost::ResolveExport(const std::wstring& module, const std::wstring& symbol, ULONG64& out) {
    ULONG idx = 0;
    ULONG64 base = 0;
    HRESULT hr = m_symbols->GetModuleByModuleName(W2A(module.c_str()).c_str(), 0, &idx, &base);
    if (FAILED(hr) || hr == S_FALSE || !base) return false;
    return ResolveExportByBase(base, symbol, out);
}

void DbgHost::ResolvePendingForModule(const std::wstring& moduleName, ULONG64 moduleBase) {
    for (auto& p : m_pending) {
        if (p.armed) continue;
        if (_wcsicmp(p.module.c_str(), moduleName.c_str()) != 0) continue;
        ULONG64 off = 0;
        if (ResolveExportByBase(moduleBase, p.symbol, off)) {
            int id = AddOffsetBreakpoint(off);
            if (m_verbose) wprintf(L"[dbg] armed pending '%s!%s' -> 0x%llx (bp %d)\n",
                p.module.c_str(), p.symbol.c_str(), (unsigned long long)off, id);
            p.armed = true;
        }
    }
}

int DbgHost::AddOffsetBreakpoint(ULONG64 offset) {
    ComPtr<IDebugBreakpoint> bp;
    HRESULT hr = m_control->AddBreakpoint(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, &bp);
    if (FAILED(hr)) return -1;
    hr = bp->SetOffset(offset);
    if (FAILED(hr)) return -1;
    bp->AddFlags(DEBUG_BREAKPOINT_ENABLED);
    ULONG id = 0;
    bp->GetId(&id);
    return (int)id;
}

void DbgHost::SetModuleBreak(const std::wstring& name) { m_moduleBreak = name; }

bool DbgHost::PumpOneEvent(DWORD timeoutMs) {
    m_last = BreakEvent{};
    HRESULT hr = m_control->WaitForEvent(DEBUG_WAIT_DEFAULT, timeoutMs);
    if (hr == S_FALSE) { if (m_verbose) wprintf(L"[dbg] WaitForEvent timeout\n"); return false; }
    if (FAILED(hr)) {
        if (m_verbose) {
            ULONG status = DEBUG_STATUS_NO_DEBUGGEE;
            ULONG pid = 0;
            m_control->GetExecutionStatus(&status);
            m_sys->GetCurrentProcessSystemId(&pid);
            wprintf(L"[dbg] WaitForEvent failed hr=0x%08x execStatus=0x%x pid=%u\n",
                hr, status, pid);
        }
        return false;
    }
    // Stepping (StepInto/StepOver) does not invoke any IDebugEventCallbacks
    // method, so if nothing else claimed this wakeup, treat it as a step stop.
    if (m_last.reason == StopReason::None) {
        BreakEvent e;
        e.reason = StopReason::Step;
        e.offset = GetRegister(L"rip");
        m_last = e;
    }
    return true;
}

void DbgHost::Go() { m_control->SetExecutionStatus(DEBUG_STATUS_GO); }
void DbgHost::BreakIn() {
    // SetInterrupt is documented for breaking a *concurrently* blocked
    // WaitForEvent from another thread; called back-to-back on the same
    // thread that then immediately calls WaitForEvent, it does not reliably
    // land. DebugBreakProcess is the standard WinAPI break-in mechanism and
    // works regardless of what the debuggee's threads are doing.
    //
    // Break into *every* process in the session, not just the engine's current
    // one. With child-following on, the process you are looking at in the CPU
    // window is often not the current one, and switching to it is only legal
    // while stopped - so a pause that only hit the current process would leave
    // the other tabs permanently unreachable. Any one process breaking stops
    // the whole session, after which the tabs can be switched freely.
    ULONG n = 0;
    if (m_sys && SUCCEEDED(m_sys->GetNumberProcesses(&n)) && n) {
        // System pids come straight out of the engine's list - no
        // SetCurrentProcessId, which would not be safe while the target runs.
        std::vector<ULONG> engineIds(n), sysIds(n);
        if (SUCCEEDED(m_sys->GetProcessIdsByIndex(0, n, engineIds.data(), sysIds.data()))) {
            bool any = false;
            for (ULONG i = 0; i < n; i++) {
                HANDLE h = ::OpenProcess(PROCESS_ALL_ACCESS, FALSE, sysIds[i]);
                if (!h) continue;
                BOOL ok = ::DebugBreakProcess(h);
                if (m_verbose) wprintf(L"[dbg] DebugBreakProcess pid=%u ok=%d err=%u\n",
                    sysIds[i], ok, ok ? 0 : GetLastError());
                ::CloseHandle(h);
                any = any || ok;
            }
            if (any) return;
        }
    }
    // Fall back to the current process (single-process session, or the
    // enumeration above failed).
    ULONG64 handle = 0;
    HRESULT hr = m_sys->GetCurrentProcessHandle(&handle);
    if (m_verbose) wprintf(L"[dbg] GetCurrentProcessHandle hr=0x%08x handle=0x%llx\n", hr, (unsigned long long)handle);
    if (SUCCEEDED(hr) && handle) {
        BOOL ok = ::DebugBreakProcess((HANDLE)handle);
        if (m_verbose) wprintf(L"[dbg] DebugBreakProcess ok=%d err=%u\n", ok, ok ? 0 : GetLastError());
    }
}
void DbgHost::StepInto() { m_control->SetExecutionStatus(DEBUG_STATUS_STEP_INTO); }
void DbgHost::StepOver() { m_control->SetExecutionStatus(DEBUG_STATUS_STEP_OVER); }

bool DbgHost::RemoveBreakpointById(ULONG id) {
    ComPtr<IDebugBreakpoint> bp;
    if (FAILED(m_control->GetBreakpointById(id, &bp))) return false;
    return SUCCEEDED(m_control->RemoveBreakpoint(bp.get()));
}

std::vector<BpInfo> DbgHost::Breakpoints() {
    std::vector<BpInfo> out;
    if (!m_control) return out;
    ULONG n = 0;
    if (FAILED(m_control->GetNumberBreakpoints(&n)) || !n) return out;
    for (ULONG i = 0; i < n; i++) {
        ComPtr<IDebugBreakpoint> bp;
        if (FAILED(m_control->GetBreakpointByIndex(i, &bp)) || !bp) continue;
        BpInfo b;
        if (FAILED(bp->GetId(&b.id))) continue;
        if (b.id == m_stepOutBpId) continue;   // ours, not the user's
        // A deferred breakpoint whose module has not loaded yet has no offset;
        // GetOffset fails and it simply has no row to paint.
        if (FAILED(bp->GetOffset(&b.addr))) continue;
        ULONG flags = 0;
        bp->GetFlags(&flags);
        b.enabled = (flags & DEBUG_BREAKPOINT_ENABLED) != 0;
        out.push_back(b);
    }
    return out;
}

ULONG64 DbgHost::GetPeb() {
    ULONG64 peb = 0;
    if (FAILED(m_sys->GetCurrentProcessPeb(&peb))) return 0;
    return peb;
}

bool DbgHost::StepOut(ULONG64& retAddr) {
    retAddr = 0;
    RegFile r = GetRegisters();
    ULONG got = 0;
    if (!ReadMemory(r.rsp, &retAddr, sizeof(retAddr), &got) || got != sizeof(retAddr) || !retAddr)
        return false;

    ClearStepOut();
    ComPtr<IDebugBreakpoint> bp;
    HRESULT hr = m_control->AddBreakpoint(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, &bp);
    if (FAILED(hr)) return false;
    hr = bp->SetOffset(retAddr);
    if (FAILED(hr)) return false;
    // GO_ONLY + ENABLED: fires only on a full go (which is how the front-end
    // implements "step out"), and not when single-stepping.
    bp->AddFlags(DEBUG_BREAKPOINT_GO_ONLY | DEBUG_BREAKPOINT_ENABLED);
    ULONG id = DEBUG_ANY_ID;
    bp->GetId(&id);
    m_stepOutBpId = id;
    if (m_verbose) wprintf(L"[dbg] step-out bp %u @ 0x%llx\n", id, (unsigned long long)retAddr);
    return true;
}

void DbgHost::ClearStepOut() {
    if (m_stepOutBpId == DEBUG_ANY_ID) return;
    ComPtr<IDebugBreakpoint> bp;
    if (SUCCEEDED(m_control->GetBreakpointById(m_stepOutBpId, &bp))) {
        bp->RemoveFlags(DEBUG_BREAKPOINT_ENABLED);
        m_control->RemoveBreakpoint(bp.get());
    }
    m_stepOutBpId = DEBUG_ANY_ID;
}

RegFile DbgHost::GetRegisters() {
    RegFile r;
    r.rax = GetRegister(L"rax");
    r.rbx = GetRegister(L"rbx");
    r.rcx = GetRegister(L"rcx");
    r.rdx = GetRegister(L"rdx");
    r.r8  = GetRegister(L"r8");
    r.r9  = GetRegister(L"r9");
    r.r10 = GetRegister(L"r10");
    r.r11 = GetRegister(L"r11");
    r.rsp = GetRegister(L"rsp");
    r.rbp = GetRegister(L"rbp");
    r.rip = GetRegister(L"rip");
    r.rdi = GetRegister(L"rdi");
    r.rsi = GetRegister(L"rsi");
    return r;
}

ULONG64 DbgHost::GetRegister(const wchar_t* name) {
    ULONG idx = 0;
    // GetValueByName is not present in the 26100 DbgEng.h; resolve name -> index.
    if (FAILED(m_regs->GetIndexByNameWide(name, &idx))) return 0;
    DEBUG_VALUE v = {};
    if (FAILED(m_regs->GetValue(idx, &v))) return 0;
    switch (v.Type) {
        case DEBUG_VALUE_INT8:   return (ULONG64)(long long)v.I8;
        case DEBUG_VALUE_INT16:  return (ULONG64)(long long)v.I16;
        case DEBUG_VALUE_INT32:  return (ULONG64)(long long)v.I32;
        case DEBUG_VALUE_INT64:  return v.I64;
        default: break;
    }
    return 0;
}

bool DbgHost::SetRegister(const wchar_t* name, ULONG64 value) {
    ULONG idx = 0;
    if (FAILED(m_regs->GetIndexByNameWide(name, &idx))) return false;
    DEBUG_VALUE v = {};
    v.Type = DEBUG_VALUE_INT64;
    v.I64 = value;
    return SUCCEEDED(m_regs->SetValue(idx, &v));
}

bool DbgHost::ReadMemory(ULONG64 addr, void* buf, ULONG size, ULONG* got) {
    if (!m_data || !addr) return false;
    return SUCCEEDED(m_data->ReadVirtual(addr, buf, size, got));
}

bool DbgHost::WriteMemory(ULONG64 addr, const void* buf, ULONG size) {
    if (!m_data || !addr) return false;
    ULONG written = 0;
    return SUCCEEDED(m_data->WriteVirtual(addr, const_cast<void*>(buf), size, &written)) && written == size;
}

bool DbgHost::Disasm(ULONG64 offset, DisasmLine& out) {
    char buf[512] = "";
    ULONG consumed = 0;
    ULONG64 next = 0;
    HRESULT hr = m_control->Disassemble(offset, 0, buf, 511, &consumed, &next);
    if (FAILED(hr)) return false;

    out.addr = offset;
    out.next = next;

    // Split the engine line "00007ffc`47592ef0 ff25f2de0500    jmp ...".
    std::vector<std::string> tok;
    char* p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char* s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        tok.push_back(std::string(s, p));
    }
    if (tok.size() >= 2 && tok[1].size() <= 32 &&
        tok[1].find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
        for (char c : tok[1]) out.bytes += (wchar_t)(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c);
        for (size_t i = 2; i < tok.size(); i++) {
            if (i > 2) out.text += L" ";
            out.text += A2W(tok[i].c_str());
        }
        if (out.text.empty()) out.text = A2W(tok[1].c_str());
        while (!out.text.empty() &&
               (out.text.back() == L'\n' || out.text.back() == L'\r' || out.text.back() == L' '))
            out.text.pop_back();
    } else {
        out.text = A2W(buf);
    }
    out.header = SymbolAt(offset);
    return true;
}

std::wstring DbgHost::SymbolAt(ULONG64 offset) {
    char buf[512] = "";
    ULONG64 disp = 0;
    if (SUCCEEDED(m_symbols->GetNameByOffset(offset, buf, 511, nullptr, &disp))) {
        std::wstring name = A2W(buf);
        if (disp) {
            wchar_t tail[64];
            swprintf_s(tail, L"+0x%llx", (unsigned long long)disp);
            return name + tail;
        }
        return name;
    }
    wchar_t hex[32];
    swprintf_s(hex, L"0x%llx", (unsigned long long)offset);
    return hex;
}

std::wstring DbgHost::ReadAscii(ULONG64 addr, ULONG max) {
    if (!addr) return L"";
    char buf[129] = {};
    ULONG got = 0;
    if (!ReadMemory(addr, buf, std::min<ULONG>(max, 128), &got)) return L"";
    std::wstring out;
    for (ULONG i = 0; i < got && buf[i]; i++) {
        out += (buf[i] >= 32 && buf[i] <= 126) ? (wchar_t)buf[i] : L'.';
    }
    return out;
}

std::wstring DbgHost::ReadWide(ULONG64 addr, ULONG max) {
    if (!addr) return L"";
    wchar_t buf[65] = {};
    ULONG got = 0;
    if (!ReadMemory(addr, buf, std::min<ULONG>(max, 128), &got)) return L"";
    buf[got / sizeof(wchar_t)] = 0;
    return std::wstring(buf);
}
