// odbg_cli - open-debugger CLI.
//
// A scriptable DbgEng front-end.  Replaces the scripted cdb calls for the
// common jobs: launch a target, wait for a module, break at symbols, dump
// registers/strings/disasm at each hit, optionally step out to capture the
// return value, and emit everything as JSON.
//
// Examples:
//   odbg_cli -launch "C:\...\target.exe"
//           -sym "srv*C:\sym*https://msdl.microsoft.com/download/symbols;C:\...\build\x64\Debug"
//           -mod plugin_host
//           -break "plugin_host!CreateInterface"
//           -break "plugin_host!Connect"
//           -strarg rcx -strarg rdx
//           -stepout
//           -json trace.json

#include "dbghost.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

static std::string JsonEscape(const std::wstring& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (wchar_t c : s) {
        unsigned long long u = (unsigned long long)c;
        if (c == L'"') out += "\\\"";
        else if (c == L'\\') out += "\\\\";
        else if (c == L'\n') out += "\\n";
        else if (c == L'\r') out += "\\r";
        else if (c == L'\t') out += "\\t";
        else if (c >= 32 && c < 127) out += (char)c;
        else { char tmp[16]; sprintf_s(tmp, "\\u%04llx", u); out += tmp; }
    }
    return out;
}

static std::string Hex64(ULONG64 v) {
    char b[32];
    sprintf_s(b, "0x%llx", (unsigned long long)v);
    return b;
}

static std::string HexBytes(const unsigned char* p, size_t n) {
    std::string s;
    char b[8];
    for (size_t i = 0; i < n; i++) { sprintf_s(b, "%02x", p[i]); s += b; }
    return s;
}

struct Options {
    std::wstring launch;
    DWORD attachPid = 0;
    std::wstring sym;
    std::wstring mod;
    std::vector<std::wstring> breaks;
    std::vector<std::wstring> strArgs;
    std::vector<std::wstring> wstrArgs;
    std::vector<std::wstring> hexDumps;
    std::vector<unsigned long> ignoreExc;
    std::wstring jsonOut;
    bool stepOut = false;
    bool verbose = false;
    bool children = false;      // follow child processes (.childdbg)
    bool breakModules = false;  // break on every module load
    bool breakThreads = false;  // break on every new thread
    int maxEvents = 100000;
};

static bool ParseArgs(int argc, wchar_t** argv, Options& o) {
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&](const wchar_t* what) -> std::wstring {
            if (i + 1 < argc) return argv[++i];
            fwprintf(stderr, L"missing value for %s\n", what);
            exit(1);
        };
        if (a == L"-launch") o.launch = next(L"-launch");
        else if (a == L"-attach") o.attachPid = (DWORD)_wtoi(next(L"-attach").c_str());
        else if (a == L"-sym") o.sym = next(L"-sym");
        else if (a == L"-mod") o.mod = next(L"-mod");
        else if (a == L"-break") o.breaks.push_back(next(L"-break"));
        else if (a == L"-strarg") o.strArgs.push_back(next(L"-strarg"));
        else if (a == L"-wstrarg") o.wstrArgs.push_back(next(L"-wstrarg"));
        else if (a == L"-hex") o.hexDumps.push_back(next(L"-hex"));
        else if (a == L"-ignoreexc") o.ignoreExc.push_back(wcstoul(next(L"-ignoreexc").c_str(), nullptr, 16));
        else if (a == L"-children") o.children = true;
        else if (a == L"-breakmodules") o.breakModules = true;
        else if (a == L"-breakthreads") o.breakThreads = true;
        else if (a == L"-json") o.jsonOut = next(L"-json");
        else if (a == L"-stepout") o.stepOut = true;
        else if (a == L"-v") o.verbose = true;
        else if (a == L"-maxevents") o.maxEvents = _wtoi(next(L"-maxevents").c_str());
        else { fwprintf(stderr, L"unknown arg: %s\n", a.c_str()); return false; }
    }
    return !o.launch.empty() || o.attachPid != 0;
}

int wmain(int argc, wchar_t** argv) {
    Options o;
    if (!ParseArgs(argc, argv, o)) {
        fwprintf(stderr,
            L"usage: odbg_cli -launch <cmdline> | -attach <pid>\n"
            L"              [-sym <sympath>] [-mod <module>] [-break <symbol>]...\n"
            L"              [-strarg <reg>]... [-wstrarg <reg>]... [-hex <reg>]... [-stepout]\n"
            L"              [-children] [-breakmodules] [-breakthreads]\n"
            L"              [-ignoreexc <hexcode>]... [-json <file>] [-maxevents <n>] [-v]\n"
            L"\n"
            L"  -children      follow child processes (OllyDbg 'debug child processes')\n"
            L"  -breakmodules  break on every module (DLL) load\n"
            L"  -breakthreads  break on every new thread\n"
            L"  -ignoreexc H   pass exception code H straight to the debuggee (repeatable)\n"
            L"  -wstrarg reg   read a UTF-16 string argument from reg (e.g. a command line)\n");
        return 1;
    }

    DbgHost host;
    if (!host.Init()) {
        fwprintf(stderr, L"Failed to initialize DbgEng (dbgeng.dll missing?)\n");
        return 1;
    }
    host.SetVerbose(o.verbose);
    host.SetFollowChildren(o.children);
    host.SetBreakOnModuleLoad(o.breakModules);
    host.SetBreakOnThreadCreate(o.breakThreads);
    for (unsigned long code : o.ignoreExc) host.AddIgnoredException(code);
    if (!o.sym.empty()) host.SetSymbolPath(o.sym);
    host.ReloadSymbols();

    if (!o.launch.empty()) {
        if (!host.Launch(o.launch)) { fwprintf(stderr, L"Launch failed\n"); return 1; }
    } else {
        if (!host.Attach(o.attachPid)) { fwprintf(stderr, L"Attach failed\n"); return 1; }
    }
    if (o.verbose) {
        if (!o.launch.empty()) wprintf(L"[main] launching '%s', entering event loop\n", o.launch.c_str());
        else wprintf(L"[main] attaching pid=%u, entering event loop\n", o.attachPid);
    }

    std::vector<std::string> events;
    int breakHits = 0;
    bool breaksAdded = false;

    for (;;) {
        if (!host.PumpOneEvent(INFINITE)) break;
        const BreakEvent& ev = host.LastEvent();

        // Breakpoints can only be created once the session has started, i.e.
        // after the first WaitForEvent returns.
        if (!breaksAdded) {
            breaksAdded = true;
            if (o.verbose) wprintf(L"[main] adding %d breakpoints\n", (int)o.breaks.size());
            for (const auto& b : o.breaks) {
                int id = host.AddDeferredBreakpoint(b);
                if (o.verbose) wprintf(L"[main] breakpoint '%s' -> id %d\n", b.c_str(), id);
            }
            if (!o.mod.empty()) host.SetModuleBreak(o.mod);
            if (!o.breaks.empty()) host.ReloadSymbols();
        }

        if (ev.reason == StopReason::ProcessExited) {
            // When following children, one process exiting (e.g. a launcher
            // handing off to the real process) must not tear the whole session
            // down while other debuggees are still live.
            ULONG remaining = o.children ? host.NumProcesses() : 0;
            if (o.children && remaining > 1) {
                events.push_back("{ \"type\": \"proc_exit\", \"remaining\": " +
                                 std::to_string(remaining - 1) + " }");
                if (o.verbose) wprintf(L"[exit] one process exited, %u still debugged\n", remaining - 1);
                host.Go();
                continue;
            }
            events.push_back("{ \"type\": \"exit\" }");
            break;
        }

        if (ev.reason == StopReason::ModuleLoaded) {
            std::string s = "{ \"type\": \"module\", \"name\": \"" +
                JsonEscape(ev.module) + "\", \"base\": \"" + Hex64(ev.moduleBase) + "\" }";
            events.push_back(s);
            if (o.verbose) wprintf(L"[mod] %s at %hs\n", ev.module.c_str(), Hex64(ev.moduleBase).c_str());
            host.Go();
            continue;
        }

        if (ev.reason == StopReason::ThreadCreated) {
            char b[96];
            sprintf_s(b, "{ \"type\": \"thread\", \"start\": \"%s\" }", Hex64(ev.offset).c_str());
            events.push_back(b);
            if (o.verbose) wprintf(L"[thread] new thread, start=%hs %ls\n",
                Hex64(ev.offset).c_str(), host.SymbolAt(ev.offset).c_str());
            host.Go();
            continue;
        }

        if (ev.reason == StopReason::Exception) {
            char b[64];
            sprintf_s(b, "{ \"type\": \"exception\", \"code\": \"0x%08x\" }", ev.exceptionCode);
            events.push_back(b);
            host.Go();
            continue;
        }

        if (ev.reason != StopReason::Breakpoint) { host.Go(); continue; }

        // A step-out breakpoint fired: we returned from the function.
        if (host.StepOutBpId() != DEBUG_ANY_ID && ev.bpId == host.StepOutBpId()) {
            RegFile r = host.GetRegisters();
            char b[256];
            sprintf_s(b,
                "{ \"type\": \"return\", \"rax\": \"%s\", \"retOffset\": \"%s\", \"symbol\": \"%s\" }",
                Hex64(r.rax).c_str(), Hex64(r.rip).c_str(),
                JsonEscape(host.SymbolAt(r.rip)).c_str());
            events.push_back(b);
            if (o.verbose) wprintf(L"[ret] rax=%hs rip=%hs\n", Hex64(r.rax).c_str(), Hex64(r.rip).c_str());
            host.ClearStepOut();
            host.Go();
            continue;
        }

        // ---- we are stopped at a breakpoint ----
        RegFile r = host.GetRegisters();

        std::string regs = "{ ";
        regs += "\"rax\": \"" + Hex64(r.rax) + "\", ";
        regs += "\"rcx\": \"" + Hex64(r.rcx) + "\", ";
        regs += "\"rdx\": \"" + Hex64(r.rdx) + "\", ";
        regs += "\"r8\": \"" + Hex64(r.r8) + "\", ";
        regs += "\"r9\": \"" + Hex64(r.r9) + "\", ";
        regs += "\"rsp\": \"" + Hex64(r.rsp) + "\", ";
        regs += "\"rbp\": \"" + Hex64(r.rbp) + "\", ";
        regs += "\"rip\": \"" + Hex64(r.rip) + "\" }";

        std::string strs = "{ ";
        bool first = true;
        for (const auto& reg : o.strArgs) {
            ULONG64 v = host.GetRegister(reg.c_str());
            if (!first) strs += ", ";
            first = false;
            strs += "\"" + JsonEscape(reg) + "\": \"" + JsonEscape(host.ReadAscii(v)) + "\"";
        }
        strs += " }";

        std::string wstrs = "{ ";
        first = true;
        for (const auto& reg : o.wstrArgs) {
            ULONG64 v = host.GetRegister(reg.c_str());
            if (!first) wstrs += ", ";
            first = false;
            wstrs += "\"" + JsonEscape(reg) + "\": \"" + JsonEscape(host.ReadWide(v)) + "\"";
        }
        wstrs += " }";

        std::string hexd = "{ ";
        first = true;
        for (const auto& reg : o.hexDumps) {
            ULONG64 v = host.GetRegister(reg.c_str());
            unsigned char buf[64] = {};
            ULONG got = 0;
            host.ReadMemory(v, buf, sizeof(buf), &got);
            if (!first) hexd += ", ";
            first = false;
            hexd += "\"" + JsonEscape(reg) + "\": \"" + HexBytes(buf, got) + "\"";
        }
        hexd += " }";

        std::string dis = "[ ";
        ULONG64 ip = r.rip;
        for (int i = 0; i < 6; i++) {
            DisasmLine dl;
            if (!host.Disasm(ip, dl)) break;
            if (i) dis += ", ";
            dis += "{ \"addr\": \"" + Hex64(dl.addr) + "\", \"header\": \"" + JsonEscape(dl.header) +
                   "\", \"bytes\": \"" + JsonEscape(dl.bytes) + "\", \"text\": \"" + JsonEscape(dl.text) + "\" }";
            if (!dl.next || dl.next == dl.addr) break;
            ip = dl.next;
        }
        dis += " ]";

        char hdr[128];
        sprintf_s(hdr, "{ \"type\": \"breakpoint\", \"bpId\": %u, \"offset\": \"%s\", \"symbol\": \"%s\", ",
            ev.bpId, Hex64(ev.offset).c_str(), JsonEscape(host.SymbolAt(ev.offset)).c_str());
        std::string evJson = hdr + std::string("\"regs\": ") + regs + ", \"strs\": " + strs +
                             ", \"hex\": " + hexd + ", \"disasm\": " + dis + " }";
        events.push_back(evJson);
        breakHits++;

        if (o.verbose) {
            wprintf(L"[bp] %u %s\n", ev.bpId, host.SymbolAt(ev.offset).c_str());
            wprintf(L"     rax=%hs rcx=%hs rdx=%hs r8=%hs r9=%hs\n",
                Hex64(r.rax).c_str(), Hex64(r.rcx).c_str(), Hex64(r.rdx).c_str(),
                Hex64(r.r8).c_str(), Hex64(r.r9).c_str());
            for (const auto& reg : o.strArgs) {
                wprintf(L"     str(%s)=%s\n", reg.c_str(), host.ReadAscii(host.GetRegister(reg.c_str())).c_str());
            }
            ULONG64 ip = r.rip;
            std::wstring prevBase;
            for (int i = 0; i < 6; i++) {
                DisasmLine dl;
                if (!host.Disasm(ip, dl)) break;
                std::wstring base = dl.header;
                size_t plus = base.rfind(L'+');
                if (plus != std::wstring::npos) {
                    bool hex = true;
                    for (size_t k = plus + 1; k < base.size(); k++) {
                        wchar_t c = base[k];
                        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == L'x')) { hex = false; break; }
                    }
                    if (hex) base = base.substr(0, plus);
                }
                if (base != prevBase) {
                    wprintf(L"     %s:\n", base.c_str());
                    prevBase = base;
                }
                wprintf(L"     %hs  %-32s %s\n", Hex64(dl.addr).c_str(), dl.bytes.c_str(), dl.text.c_str());
                if (!dl.next || dl.next == dl.addr) break;
                ip = dl.next;
            }
        }

        if (breakHits >= o.maxEvents) break;

        if (o.stepOut) {
            ULONG64 retAddr = 0;
            if (host.StepOut(retAddr)) continue;
        }
        host.Go();
    }

    host.ClearStepOut();

    if (!o.jsonOut.empty()) {
        FILE* f = nullptr;
        _wfopen_s(&f, o.jsonOut.c_str(), L"w");
        if (f) {
            fwprintf(f, L"[\n");
            for (size_t i = 0; i < events.size(); i++) {
                fwprintf(f, L"  %S%s\n", events[i].c_str(), i + 1 < events.size() ? L"," : L"");
            }
            fwprintf(f, L"]\n");
            fclose(f);
        }
    }

    wprintf(L"%zu events, %d breakpoints hit\n", events.size(), breakHits);
    return 0;
}
