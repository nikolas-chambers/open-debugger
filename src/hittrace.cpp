#include "hittrace.h"

namespace {

// Longest straight-line run we will sweep from one stop before giving up. A
// runaway sweep means we misdecoded something and are walking garbage; bailing
// out loses coverage but never wedges the trace.
const int kMaxSweep = 8192;

inline long long Rel8(const unsigned char* p) { return (long long)(signed char)p[0]; }
inline long long Rel32(const unsigned char* p) {
    unsigned int v = (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
                     ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
    return (long long)(int)v;
}

} // namespace

FlowInfo ClassifyFlow(const unsigned char* b, size_t len, ULONG64 next) {
    FlowInfo f;
    if (!b || len == 0) return f;

    // Step over legacy prefixes and REX. None of them change which opcode means
    // "branch", but they do shift where the opcode byte sits.
    size_t i = 0;
    for (; i < len; i++) {
        unsigned char c = b[i];
        if (c == 0xF0 || c == 0xF2 || c == 0xF3 ||          // lock / rep
            c == 0x2E || c == 0x36 || c == 0x3E || c == 0x26 ||  // segment
            c == 0x64 || c == 0x65 ||                        // fs / gs
            c == 0x66 || c == 0x67)                          // operand / address size
            continue;
        if (c >= 0x40 && c <= 0x4F) continue;                // REX
        break;
    }
    if (i >= len) return f;

    unsigned char op = b[i];
    size_t rest = len - i - 1;          // bytes available after the opcode
    const unsigned char* arg = b + i + 1;

    // Two-byte opcodes: only the near jcc family matters here.
    if (op == 0x0F) {
        if (rest < 1) return f;
        unsigned char op2 = arg[0];
        if (op2 >= 0x80 && op2 <= 0x8F) {                    // jcc rel32
            if (rest < 5) return f;
            f.kind = FlowKind::JccDirect;
            f.target = next + (ULONG64)Rel32(arg + 1);
        }
        // 0F 05 syscall returns to the following instruction, so it stays Linear.
        return f;
    }

    if (op >= 0x70 && op <= 0x7F) {                          // jcc rel8
        if (rest < 1) return f;
        f.kind = FlowKind::JccDirect;
        f.target = next + (ULONG64)Rel8(arg);
        return f;
    }

    switch (op) {
    case 0xEB:                                               // jmp rel8
        if (rest < 1) return f;
        f.kind = FlowKind::JmpDirect;
        f.target = next + (ULONG64)Rel8(arg);
        return f;
    case 0xE9:                                               // jmp rel32
        if (rest < 4) return f;
        f.kind = FlowKind::JmpDirect;
        f.target = next + (ULONG64)Rel32(arg);
        return f;
    case 0xE8:                                               // call rel32
        if (rest < 4) return f;
        f.kind = FlowKind::CallDirect;
        f.target = next + (ULONG64)Rel32(arg);
        return f;
    case 0xE0: case 0xE1: case 0xE2:                         // loopne / loope / loop
    case 0xE3:                                               // jrcxz
        if (rest < 1) return f;
        f.kind = FlowKind::JccDirect;
        f.target = next + (ULONG64)Rel8(arg);
        return f;
    case 0xC2: case 0xC3: case 0xCA: case 0xCB:              // ret
        f.kind = FlowKind::Ret;
        return f;
    case 0xCC: case 0xCD:                                    // int3 / int n
        // Control leaves in a way we cannot follow; stop the sweep rather than
        // walk into whatever follows the trap.
        f.kind = FlowKind::JmpIndirect;
        return f;
    case 0xFF: {
        if (rest < 1) return f;
        unsigned char reg = (unsigned char)((arg[0] >> 3) & 7);
        if (reg == 2 || reg == 3) f.kind = FlowKind::CallIndirect;  // call r/m
        else if (reg == 4 || reg == 5) f.kind = FlowKind::JmpIndirect;  // jmp r/m
        return f;
    }
    default:
        return f;
    }
}

void HitTrace::Start(Host* host, ULONG64 startAddr) {
    m_host = host;
    m_active = true;
    WalkFrom(startAddr);
}

void HitTrace::Stop() {
    if (m_host) {
        for (const auto& kv : m_armed) m_host->DisarmBp(kv.second);
    }
    m_armed.clear();
    m_armedIds.clear();
    m_active = false;
}

void HitTrace::Clear() {
    Stop();
    m_executed.clear();
    m_blocks = 0;
}

void HitTrace::Arm(ULONG64 addr) {
    if (!addr || !m_host) return;
    if (m_executed.count(addr)) return;     // already walked through here
    if (m_armed.count(addr)) return;        // already waiting on it
    int id = m_host->ArmBp(addr);
    if (id >= 0) {
        m_armed[addr] = id;
        m_armedIds.insert(id);
    }
}

bool HitTrace::OnStop(ULONG64 addr) {
    if (!m_active) return false;
    auto it = m_armed.find(addr);
    if (it == m_armed.end()) return false;  // somebody else's breakpoint

    m_host->DisarmBp(it->second);
    m_armedIds.erase(it->second);
    m_armed.erase(it);
    WalkFrom(addr);
    return true;
}

void HitTrace::WalkFrom(ULONG64 addr) {
    if (!m_host) return;
    ULONG64 cur = addr;

    for (int n = 0; n < kMaxSweep; n++) {
        // Reaching code we have already swept means this path merges into a run
        // we have accounted for; everything beyond is already covered.
        if (m_executed.count(cur)) return;
        m_executed.insert(cur);

        unsigned char code[16] = {};
        ULONG got = 0;
        if (!m_host->ReadCode(cur, code, sizeof(code), &got) || got == 0) return;

        ULONG64 next = 0;
        if (!m_host->NextInstr(cur, next) || next <= cur) return;

        FlowInfo f = ClassifyFlow(code, got, next);
        switch (f.kind) {
        case FlowKind::Linear:
            cur = next;
            continue;

        case FlowKind::JmpDirect:
            m_blocks++;
            Arm(f.target);
            return;

        case FlowKind::JccDirect:
        case FlowKind::CallDirect:
            // Both successors are real: the branch/call target, and the
            // instruction after it (the not-taken path, or the return site).
            m_blocks++;
            Arm(f.target);
            Arm(next);
            return;

        case FlowKind::CallIndirect:
            // Target is unknowable, but the call still comes back to `next`, so
            // arming it keeps the sweep alive past the call.
            m_blocks++;
            Arm(next);
            return;

        case FlowKind::JmpIndirect:
        case FlowKind::Ret:
            // Nothing statically knowable. A ret lands on the return site the
            // matching call already armed, so this is not a dead end in practice.
            m_blocks++;
            return;
        }
    }
}
