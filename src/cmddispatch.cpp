#include "cmddispatch.h"

#include <sstream>
#include <vector>
#include <algorithm>
#include <cwctype>

namespace {

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

std::vector<std::wstring> Tokenize(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstringstream ss(s);
    std::wstring tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

std::string W2A(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string a(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, &a[0], n, nullptr, nullptr);
    return a;
}

ULONG64 ParseHex(const std::wstring& s) {
    return _wcstoui64(s.c_str(), nullptr, 16);
}

std::string Hex64(ULONG64 v) {
    char b[32];
    sprintf_s(b, "0x%llx", (unsigned long long)v);
    return b;
}

bool ParseHexBytes(const std::wstring& s, std::vector<unsigned char>& out) {
    std::wstring hex;
    for (wchar_t c : s) if (c != L' ') hex += c;
    if (hex.size() % 2 != 0) return false;
    for (size_t i = 0; i < hex.size(); i += 2) {
        wchar_t hi = hex[i], lo = hex[i + 1];
        auto val = [](wchar_t c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = val(hi), l = val(lo);
        if (h < 0 || l < 0) return false;
        out.push_back((unsigned char)((h << 4) | l));
    }
    return true;
}

} // namespace

DispatchResult DispatchCommand(DbgHost& host, const std::wstring& cmdLine,
                                bool sessionActive, bool stopped) {
    DispatchResult r;
    auto tok = Tokenize(cmdLine);
    if (tok.empty()) { r.ok = false; r.output = "(empty command)"; return r; }

    std::wstring verb = ToLower(tok[0]);

    if (verb == L"launch") {
        if (sessionActive) { r.ok = false; r.output = "a session is already active"; return r; }
        size_t pos = cmdLine.find(tok[0]);
        std::wstring rest = cmdLine.substr(pos + tok[0].size());
        rest.erase(0, rest.find_first_not_of(L' '));
        if (rest.empty()) { r.ok = false; r.output = "usage: launch <cmdline>"; return r; }
        r.ok = host.Launch(rest);
        r.sessionStarted = r.ok;
        r.output = r.ok ? ("launching: " + W2A(rest)) : "launch failed";
        return r;
    }

    if (verb == L"attach") {
        if (sessionActive) { r.ok = false; r.output = "a session is already active"; return r; }
        if (tok.size() < 2) { r.ok = false; r.output = "usage: attach <pid>"; return r; }
        DWORD pid = (DWORD)_wtoi(tok[1].c_str());
        r.ok = host.Attach(pid);
        r.sessionStarted = r.ok;
        r.output = r.ok ? ("attaching to pid " + std::to_string(pid)) : "attach failed";
        return r;
    }

    if (verb == L"restart") {
        // Works whether or not a session is live: with one, it kills and re-runs;
        // without one, it re-runs the last target this host was given.
        r.ok = host.Restart();
        r.sessionStarted = r.ok;
        r.sessionEnded = !r.ok;
        r.output = r.ok ? "restarted" : "nothing to restart (no previous target)";
        return r;
    }

    if (verb == L"kill") {
        if (!sessionActive) { r.ok = false; r.output = "no active session"; return r; }
        host.Kill();
        r.sessionEnded = true;
        r.output = "process killed";
        return r;
    }

    // ---- Options (OllyDbg-style), settable with or without a live session ----
    auto onOff = [&](bool cur) -> bool {
        if (tok.size() < 2) return !cur;                 // bare verb toggles
        std::wstring v = ToLower(tok[1]);
        return v == L"1" || v == L"on" || v == L"true" || v == L"yes";
    };

    if (verb == L"childdbg") {
        bool on = onOff(host.FollowChildren());
        host.SetFollowChildren(on);
        r.output = std::string("debug child processes: ") + (on ? "ON" : "off");
        return r;
    }
    if (verb == L"breakmod") {
        bool on = onOff(host.BreakOnModuleLoad());
        host.SetBreakOnModuleLoad(on);
        r.output = std::string("break on new module: ") + (on ? "ON" : "off");
        return r;
    }
    if (verb == L"breakthread") {
        bool on = onOff(host.BreakOnThreadCreate());
        host.SetBreakOnThreadCreate(on);
        r.output = std::string("break on new thread: ") + (on ? "ON" : "off");
        return r;
    }
    if (verb == L"ignoreexc") {
        // Accepts a single code or an inclusive range "<lo>-<hi>".
        if (tok.size() < 2) { r.ok = false; r.output = "usage: ignoreexc <hex> | <lo>-<hi>"; return r; }
        size_t dash = tok[1].find(L'-');
        if (dash != std::wstring::npos) {
            ULONG lo = (ULONG)ParseHex(tok[1].substr(0, dash));
            ULONG hi = (ULONG)ParseHex(tok[1].substr(dash + 1));
            host.AddIgnoredExceptionRange(lo, hi);
            r.output = "ignoring exceptions " + Hex64(lo) + "-" + Hex64(hi);
        } else {
            ULONG code = (ULONG)ParseHex(tok[1]);
            host.AddIgnoredException(code);
            r.output = "ignoring exception " + Hex64(code);
        }
        return r;
    }
    if (verb == L"catchexc") {
        if (tok.size() < 2) { r.ok = false; r.output = "usage: catchexc <hexcode>"; return r; }
        ULONG code = (ULONG)ParseHex(tok[1]);
        host.RemoveIgnoredException(code);
        r.output = "no longer ignoring exception " + Hex64(code);
        return r;
    }
    if (verb == L"rmexc") {
        if (tok.size() < 2) { r.ok = false; r.output = "usage: rmexc <row-index>"; return r; }
        size_t idx = (size_t)_wtoi(tok[1].c_str());
        host.RemoveIgnoredExceptionAt(idx);
        r.output = "removed ignored-exception row " + std::to_string(idx);
        return r;
    }
    if (verb == L"exceptions" || verb == L"sx") {
        std::string out = "ignored exceptions:";
        const auto& list = host.IgnoredExceptions();
        if (list.empty()) out += " (none)";
        else for (const auto& e : list)
            out += " " + (e.lo == e.hi ? Hex64(e.lo) : Hex64(e.lo) + "-" + Hex64(e.hi));
        r.output = out;
        return r;
    }

    if (!sessionActive) { r.ok = false; r.output = "no active session (use launch/attach first)"; return r; }

    if (verb == L"bp" || verb == L"bpx") {
        if (tok.size() < 2) { r.ok = false; r.output = "usage: bp <module!symbol|addr>"; return r; }
        int id;
        if (tok[1].find(L'!') != std::wstring::npos) {
            id = host.AddDeferredBreakpoint(tok[1]);
        } else {
            id = host.AddOffsetBreakpoint(ParseHex(tok[1]));
        }
        r.ok = id >= 0;
        r.output = r.ok ? ("breakpoint " + std::to_string(id) + " set") : "failed to set breakpoint";
        return r;
    }

    if (verb == L"bc") {
        if (tok.size() < 2) { r.ok = false; r.output = "usage: bc <id>"; return r; }
        ULONG id = (ULONG)_wtoi(tok[1].c_str());
        r.ok = host.RemoveBreakpointById(id);
        r.output = r.ok ? ("breakpoint " + std::to_string(id) + " cleared") : "no such breakpoint";
        return r;
    }

    if (verb == L"g") {
        if (!stopped) { r.ok = false; r.output = "target is already running"; return r; }
        host.Go();
        r.resumed = true;
        r.output = "go";
        return r;
    }

    if (verb == L"p") {
        if (!stopped) { r.ok = false; r.output = "target is already running"; return r; }
        host.StepOver();
        r.resumed = true;
        r.output = "step over";
        return r;
    }

    if (verb == L"t") {
        if (!stopped) { r.ok = false; r.output = "target is already running"; return r; }
        host.StepInto();
        r.resumed = true;
        r.output = "step into";
        return r;
    }

    if (verb == L"pause") {
        if (stopped) { r.ok = false; r.output = "already stopped"; return r; }
        host.BreakIn();
        r.output = "break requested";
        return r;
    }

    if (verb == L"rtr") {
        if (!stopped) { r.ok = false; r.output = "target is already running"; return r; }
        ULONG64 retAddr = 0;
        r.ok = host.StepOut(retAddr);
        if (r.ok) host.Go();  // StepOut() only sets the breakpoint; caller must resume.
        r.resumed = r.ok;
        r.output = r.ok ? ("run to return @ " + Hex64(retAddr)) : "step-out failed (bad stack?)";
        return r;
    }

    // Resolve a token that may be a hex address or a register name.
    // If it starts with a digit (incl. 0x), parse as hex; otherwise look it up
    // as a register name.  This lets the user type "u rip" or "d rsp" the same
    // way OllyDbg does.
    auto resolveAddr = [&](const std::wstring& s) -> ULONG64 {
        if (s.empty()) return 0;
        if (iswdigit(s[0])) return ParseHex(s);
        return host.GetRegister(s.c_str());
    };

    if (verb == L"u") {
        ULONG64 addr = tok.size() >= 2 ? resolveAddr(tok[1]) : host.GetRegister(L"rip");
        r.viewKind = ViewKind::Disasm;
        r.viewAddr = addr;
        r.output = "view disasm @ " + Hex64(addr);
        return r;
    }

    if (verb == L"d" || verb == L"db" || verb == L"dw" || verb == L"dd") {
        ULONG64 addr = tok.size() >= 2 ? resolveAddr(tok[1]) : host.GetRegister(L"rsp");
        r.viewKind = ViewKind::Dump;
        r.viewAddr = addr;
        r.dumpWidth = verb == L"dw" ? DumpWidth::Word : verb == L"dd" ? DumpWidth::Dword : DumpWidth::Byte;
        r.output = "view dump @ " + Hex64(addr);
        return r;
    }

    if (verb == L"eb") {
        if (tok.size() < 3) { r.ok = false; r.output = "usage: eb <addr> <hexbytes>"; return r; }
        ULONG64 addr = resolveAddr(tok[1]);
        std::vector<unsigned char> bytes;
        if (!ParseHexBytes(tok[2], bytes)) { r.ok = false; r.output = "bad hex byte string"; return r; }
        r.ok = host.WriteMemory(addr, bytes.data(), (ULONG)bytes.size());
        r.output = r.ok ? ("wrote " + std::to_string(bytes.size()) + " bytes @ " + Hex64(addr))
                        : "write failed";
        return r;
    }

    if (verb == L"r") {
        if (tok.size() == 1) {
            RegFile reg = host.GetRegisters();
            char buf[512];
            sprintf_s(buf,
                "rax=%llx rbx=%llx rcx=%llx rdx=%llx r8=%llx r9=%llx rsp=%llx rbp=%llx rip=%llx",
                reg.rax, reg.rbx, reg.rcx, reg.rdx, reg.r8, reg.r9, reg.rsp, reg.rbp, reg.rip);
            r.output = buf;
            return r;
        }
        if (tok.size() == 2) {
            ULONG64 v = host.GetRegister(tok[1].c_str());
            r.output = W2A(tok[1]) + " = " + Hex64(v);
            return r;
        }
        // r <reg> <value>
        ULONG64 v = ParseHex(tok[2]);
        r.ok = host.SetRegister(tok[1].c_str(), v);
        r.output = r.ok ? (W2A(tok[1]) + " set to " + Hex64(v)) : "failed to set register";
        return r;
    }

    r.ok = false;
    r.output = "unknown command: " + W2A(tok[0]);
    return r;
}
