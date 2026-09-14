#pragma once
// Hit trace (OllyDbg 2.0 style): discover which code actually executes, without
// single-stepping and without relying on prior analysis.
//
// OllyDbg 1.10 implemented this by replacing every recognized instruction with
// INT3, which fell apart on non-trivial programs. Version 2 instead sets
// breakpoints dynamically on the branches it has not yet seen taken, and drops
// each one the first time it is hit. The debuggee therefore runs at full native
// speed between discoveries, and the breakpoint population drains towards zero
// as coverage converges - Olly notes 20-30k live breakpoints is not a problem.
//
// The engine cost of a stop is what makes this worth doing: on this stack a
// single-step round trip measures ~185us, so instruction-level tracing caps out
// around 5k instructions/sec. Hit trace pays that price once per basic block
// *first visit* and nothing at all thereafter.
//
// The walk is deliberately static-analysis-free: from a confirmed-executed
// address we sweep forward until the first control transfer, which is sound
// because straight-line code between two branches either all executes or none
// of it does. Only at the branch do we have to guess, and there we guess only
// what the encoding tells us for certain.

#include <windows.h>

#include <unordered_map>
#include <unordered_set>

// What a decoded instruction does to control flow. Only the cases we can settle
// from the encoding alone appear here; anything else is Linear.
enum class FlowKind {
    Linear,        // falls through to the next instruction
    JmpDirect,     // unconditional, target known
    JccDirect,     // conditional, target known; also falls through
    CallDirect,    // target known; returns to the following instruction
    CallIndirect,  // target unknowable, but it still returns to the next instruction
    JmpIndirect,   // target unknowable, no fallthrough
    Ret,           // returns to the caller (which armed the return site already)
};

struct FlowInfo {
    FlowKind kind = FlowKind::Linear;
    ULONG64  target = 0;   // meaningful for the *Direct kinds
};

// Classify one instruction. `next` is the address of the following instruction
// (x86 relative branches are relative to the *end* of the instruction, so with
// `next` in hand the target is just next + displacement). `len` is how many
// bytes of `bytes` are valid.
FlowInfo ClassifyFlow(const unsigned char* bytes, size_t len, ULONG64 next);

class HitTrace {
public:
    // Everything the trace needs from the debug engine. DbgHost implements it;
    // keeping it abstract means the walk logic stays testable without a live
    // debuggee, and does not drag dbgeng into this translation unit.
    struct Host {
        virtual ~Host() = default;
        virtual bool ReadCode(ULONG64 addr, unsigned char* buf, ULONG size, ULONG* got) = 0;
        virtual bool NextInstr(ULONG64 addr, ULONG64& next) = 0;
        virtual int  ArmBp(ULONG64 addr) = 0;   // breakpoint id, or -1
        virtual void DisarmBp(int id) = 0;
    };

    void Start(Host* host, ULONG64 startAddr);
    void Stop();
    bool Active() const { return m_active; }

    // Called for every breakpoint stop. Returns true if the stop belonged to
    // the hit trace, in which case the caller should resume without surfacing a
    // pause to the user - this is the hot path and must not touch the GUI.
    bool OnStop(ULONG64 addr);

    // True if this breakpoint id is one of ours, so the CPU window neither
    // paints it as a user breakpoint nor lets it be cleared by clicking.
    bool OwnsBp(int id) const { return m_armedIds.count(id) != 0; }

    bool WasExecuted(ULONG64 addr) const { return m_executed.count(addr) != 0; }

    size_t  ExecutedCount() const { return m_executed.size(); }
    size_t  ArmedCount() const { return m_armed.size(); }
    ULONG64 BlocksWalked() const { return m_blocks; }

    void Clear();

private:
    void WalkFrom(ULONG64 addr);
    void Arm(ULONG64 addr);

    Host* m_host = nullptr;
    bool  m_active = false;

    std::unordered_set<ULONG64>   m_executed;   // instruction addresses confirmed run
    std::unordered_map<ULONG64, int> m_armed;   // address -> breakpoint id, awaiting first hit
    std::unordered_set<int>       m_armedIds;   // the same ids, for OwnsBp()
    ULONG64 m_blocks = 0;                       // basic blocks discovered
};
