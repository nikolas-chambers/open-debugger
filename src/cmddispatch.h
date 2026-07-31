#pragma once
// Shared Olly-style command-line dispatcher. One vocabulary, usable from the
// GUI's command bar, the named-pipe channel, and (future) a CLI REPL.
//
// Verbs (all case-insensitive):
//   launch <cmdline>          start a fresh target
//   attach <pid>               attach to a running process
//   kill                       terminate the target and end the session
//   restart                    kill and re-run the same target from the top
//   proc <engineId>            make another debugged process current (CPU tabs)
//   closeproc <engineId>       drop an exited process's tab from the CPU window
//   bp / bpx <module!sym|addr> set a breakpoint
//   bc <id>                    clear a breakpoint by id
//   g                          go / resume
//   pause                      break in (interrupt) a running target
//   p                          step over
//   t                          step into
//   rtr                        step out (run to return)
//   u [addr]                   move the disassembly view to addr (default rip)
//   d|db|dw|dd [addr]          move the dump view to addr, in that unit width
//   eb <addr> <hexbytes>       patch memory
//   r [reg] [value]            show registers, or set one register

#include "dbghost.h"
#include <string>

enum class DumpWidth { Byte, Word, Dword };

enum class ViewKind { None, Disasm, Dump };

struct DispatchResult {
    bool ok = true;
    bool resumed = false;   // true if this command resumed execution (g/p/t/rtr)
    bool sessionStarted = false;
    bool sessionEnded = false;  // true if this command killed the target (kill)
    std::string output;     // human-readable result/echo, for the log pane

    // Set by u/d/db/dw/dd: tells the caller to move a view's cursor.
    ViewKind viewKind = ViewKind::None;
    ULONG64 viewAddr = 0;
    DumpWidth dumpWidth = DumpWidth::Byte;
};

// `host` must already be Init()'d. Some verbs (launch/attach) are only valid
// when no session is active; others require a live, stopped target.
DispatchResult DispatchCommand(DbgHost& host, const std::wstring& cmdLine,
                                bool sessionActive, bool stopped);
