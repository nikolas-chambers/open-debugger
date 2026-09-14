# Controlling open-debugger externally (odbg.exe)

The GUI exposes a **named-pipe command channel** (`\\.\pipe\odbg_cmd`) so an
outside script can drive the exact same debug session the CPU window renders.
One process is debugged per session; every DbgEng call runs on the GUI's
single worker thread, and each pipe command is request/response (the reply is
the command's output).

## Quick start

```powershell
# Start the GUI (it creates the pipe on startup):
C:\PROJECTS\open-debugger\build\Release\odbg.exe

# Drive it — commands go in on stdin, one per line:
"exceptions" | python C:\PROJECTS\open-debugger\tools\odbg_pipe.py
```

The helper `tools/odbg_pipe.py` opens the pipe in message mode, writes
`<cmd>\n`, and prints the reply. Without it you can talk to the pipe directly
(see `odbg_pipe.py` for the CreateFileW/WriteFile/ReadFile incantation).
`python tools\odbg_pipe.py stress <n>` hammers the pipe to shake out races.

## Command reference

Verbs are case-insensitive; numbers are hex unless noted.

| Command | Meaning |
|---|---|
| `launch <cmdline>` | CreateProcess a new target and break at entry. Fails if a session is already active. |
| `attach <pid>` | Attach to a running process. Fails if a session is already active. |
| `restart` | Kill the current target and relaunch the last one (works even with no live session if a target was ever launched). |
| `kill` | Terminate the target. |
| `bp <module!symbol>` | Deferred breakpoint, resolved when the module loads. |
| `bp <hexaddr>` | Breakpoint at an absolute address. |
| `bc <id>` | Remove a breakpoint by id. |
| `he <addr>` | Hardware breakpoint on execute (debug register, not INT3 - survives self-modifying code and unpackers that overwrite the byte). |
| `hr <addr> [size]` | Hardware breakpoint on read (size 1/2/4/8, default 1). |
| `hw <addr> [size]` | Hardware breakpoint on write. |
| `hd <id>` | Remove a hardware breakpoint by id. |
| `g` / `run` | Continue (requires the target to be stopped). |
| `ge` | Continue, passing the current first-chance exception to the debuggee's own handler (OllyDbg's Shift+F9). |
| `so` (alias `p`) | Step over (requires stopped). |
| `si` / `s` | Step into (requires stopped). |
| `tr` (alias `rtr`) | Run to return address (requires stopped). |
| `pause` / `stop` | Async break-in. Returns immediately; the engine stops shortly after. |
| `reg` (alias `r`) | Print all registers (rax..rip). |
| `reg <name>` | Print one register (e.g. `reg rip`, `reg rbx`). |
| `reg <name> <hex>` | Write a register (e.g. `reg rbx 0x123456789`). |
| `poke <addr> <hexbytes>` (alias `eb`) | Write memory. **No spaces in the byte string** — the tokenizer splits on whitespace, so `poke rsp 41424344` writes 4 bytes; `poke rsp 41 42` writes 1. |
| `eval <expr>` (aliases `?`, `calc`) | Evaluate a DbgEng expression to a value (e.g. `eval rip+10`, `? kernel32!CreateFileW`, `eval poi(rsp)`). |
| `u` / `at` / `follow [addr\|reg]` | Point the disasm view (GUI CPU window) at an address (default: `rip`). |
| `orig` / `*` | Point the disasm view at the instruction pointer. |
| `d` / `db` / `dw` / `dd [addr\|reg]` | Point the dump view at an address (default: `rsp`). |
| `memmap` | Refresh the Memory Map window's region list (walks the address space; needs a stopped target). |
| `ht [on\|off]` | Hit trace: full-speed coverage discovery (bare verb reports status). |
| `htclear` | Clear accumulated hit-trace coverage. |
| `breakentry [on\|off]` | First pause at the main module's entry point (OllyDbg 2 / x64dbg style) instead of the ntdll system breakpoint. Launch/restart only. |
| `childdbg [on\|off]` | Follow / debug child processes (bare verb toggles). |
| `breakmod [on\|off]` | Break on new module load. |
| `breakthread [on\|off]` | Break on new thread create. |
| `ignoreexc <hex>` | Ignore an exception code (e.g. `ignoreexc c0000005`). |
| `ignoreexc <lo>-<hi>` | Ignore an exception code range. |
| `catchexc <hex>` | Stop ignoring an exception code. |
| `rmexc <index>` | Remove an ignored-exception row by index. |
| `exceptions` (or `sx`) | List currently ignored exceptions. |

**Verb naming.** Verbs follow OllyDbg where Olly has one (`si`/`so`/`tr`/`ge`/
`?`/`at`/`orig`), with clear odbg names where it does not (`reg`, `poke`,
`launch`, `attach`, `ht`). Legacy short forms (`r`, `eb`, `p`, `rtr`) stay as
aliases. **One deliberate break from earlier odbg builds:** `t` no longer means
step-into — in OllyDbg `t` is run-trace, so it is reserved for that and, until
the trace engine lands, returns an error rather than silently stepping. Use
`si`/`s` to step into.

`addr`/`reg` tokens: a token starting with a digit is parsed as hex, anything
else is looked up as a register name (`u rip`, `d rsp`, `poke rsp 41424344` all
work).

## Gotchas learned while driving it

- **The pipe reads commands from stdin, not argv.** `odbg_pipe.py "launch x"`
  does nothing; `"launch x" | python odbg_pipe.py` works.
- **`r` returns all-zero registers while the target is running.** The DbgEng
  calls are marshalled to the GUI's worker thread; `GetRegisters` on a running
  target comes back zero. If `r` gives zeros after `g`/`pause`, the target
  isn't stopped yet — poll or `restart`.
- **`pause` is asynchronous.** The reply is "break requested"; the stop event
  lands a moment later. Give it a beat before reading registers.
- **`restart` is the reliable way back to a clean stopped state** — it kills
  and relaunches, breaking at entry, so registers are valid immediately.
- **`bp <hexaddr>` on ntdll's initial-breakpoint region gets consumed by the
  entry break** and the target keeps running — don't expect it to hold.
- **`launch`/`attach` reject a second session** ("a session is already
  active"); use `kill`/`restart` first.
- **Hardware breakpoints (`he`/`hr`/`hw`) do not arm at the initial loader
  break.** DbgEng does not program the debug registers for a HW breakpoint set
  at the very first stop (WinDbg's `ba` behaves the same). Set them after the
  target has started executing — e.g. run to a software breakpoint first, then
  arm the HW breakpoint. This is exactly the unpacking flow: run to the packer
  stub with `bp`, then `he <OEP>`; the HW breakpoint survives the stub
  decompressing over the OEP (an INT3 there would be overwritten).
- **Only four hardware breakpoints exist** (four debug registers); the fifth
  fails. `hd <id>` frees one.
- The `u`/`d`/`exceptions` commands mutate GUI state; the disasm/dump *bytes*
  are only rendered in the window, not returned in the pipe reply.

## Verified control sequence (tested against build\Release\odbg.exe)

```
launch C:\Windows\System32\notepad.exe   -> launching: ...
reg                                     -> rax=0 rbx=... rip=7ffd2889cb44
reg rbx 0x123456789                     -> rbx set to 0x123456789
reg rbx                                 -> rbx = 0x123456789
eval rip+10                             -> rip+10 = 0x... (...)
bp 0x7ffd2889cb44                       -> breakpoint 0 set
g                                       -> go
pause                                   -> break requested
restart                                 -> restarted   (re-breaks at entry)
poke rsp 41424344                       -> wrote 4 bytes @ ...
si                                      -> step into
kill                                    -> process killed
```

The legacy short forms (`r`, `eb`, `p`) still work as aliases, so older scripts
keep running — except `t`, which is now reserved for run-trace (use `si`).

Build/launch tasks live in `open-debugger.code-workspace` (Configure/Build
open-debugger x64, then `odbg (GUI, x64 Release)`).
