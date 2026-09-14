# open-debugger audit — where we stand vs. the standards

A standing, foundation-up comparison of odbg against the debuggers we measure
ourselves by. **Rule:** every feature is benchmarked against the field; gaps are
tracked here and in [ROADMAP.md](ROADMAP.md). Keep this current as features land.

Legend: **✅** have · **🟡** partial · **❌** missing · **—** n/a.
Reference tools: **O1** OllyDbg 1.10 · **O2** OllyDbg 2.01 · **x64** x64dbg ·
**IDA** IDA Pro · **WD** WinDbg · **G** Ghidra. (32-bit-only tools O1/O2 are
scored on their own turf; odbg's edge is 64-bit + scriptability.)

This audit reflects odbg as of the current branch. It is deliberately blunt
about what is missing — that is the point.

---

## Scorecard (headline)

| Area | odbg | Best-in-class | Gap size |
|---|---|---|---|
| Core execution control | ✅ solid | all | small |
| Breakpoints | 🟡 sw + hw + entry | x64/O2 (cond/log/mem) | medium |
| Tracing | 🟡 hit trace only | O2/x64 (run trace) | **large** |
| Disassembly | ✅ (DbgEng) | IDA | small (engine ok) |
| **Analysis** (procs/loops/args) | ❌ | IDA/O2 | **largest** |
| Decompiler | ❌ | IDA/G | **large** (optional) |
| Registers/context view | 🟡 13 GP only | all | medium |
| Memory view/edit | 🟡 hex byte/word/dword | all | medium |
| Patching | 🟡 `poke` only | O1/x64 | medium |
| Symbols | ✅ strong | WD/x64 | small |
| Modules/threads/handles | 🟡 Modules only | all | medium |
| Navigation & search | ❌ | all | **large** |
| Annotations (labels/comments/watch) | ❌ | all | medium |
| Anti-anti-debug | 🟡 plugin (PEB) | x64(ScyllaHide) | medium |
| Unpacking/dump/IAT | 🟡 HW-bp OEP proven | x64(Scylla) | medium |
| Scripting/automation | ✅ pipe + CLI + Odbg_Command | x64/WD/IDA | **odbg leads** |
| Plugins | ✅ SDK, full surface | O1/x64/IDA | small |
| UI/windows | 🟡 shell + 4 real | O2/x64 | medium |
| Persistence (.udd/project) | ❌ | O1/O2/IDA/G | **large** |
| Platform (64-bit) | ✅ | x64/WD/IDA/G | **odbg vs O1/O2 win** |

Three gaps dominate: **analysis**, **tracing**, and **navigation/search**.
Two "remembers your work" gaps hurt daily use: **persistence** and
**annotations**. odbg already *leads* on scriptability.

---

## 1. Platform & targets
| | odbg | O1 | O2 | x64 | WD | IDA | G |
|---|---|---|---|---|---|---|---|
| 64-bit user-mode | ✅ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ |
| 32-bit user-mode | 🟡 builds, ❌ debugs | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Kernel debugging | ❌ | ❌ | ❌ | 🟡 | ✅ | 🟡 | — |
| Crash dumps (minidump) | ❌ (engine can) | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ |
| Remote debugging | ❌ (DbgEng can) | ❌ | ❌ | 🟡 | ✅ | ✅ | ✅ |

**Gap:** 32-bit debugging is table-stakes (O1/O2 are 32-bit-only, so most Olly
targets/tutorials are x86). Register model is x64-hardcoded — fix unlocks it.
Minidump load is nearly free on DbgEng and worth having.

## 2. Execution control
| | odbg | field |
|---|---|---|
| Run / go, go-till-address | ✅ `g`/`run` (till-addr via bp) | all |
| Pass exception to handler | ✅ `ge` | O1/O2/x64/WD |
| Step into / over / out | ✅ `si`/`so`/`tr` | all |
| Run to selection | ✅ (bp) | all |
| Execute till user code (Alt+F9) | ❌ | O1/O2/x64 |
| Animate into/over | ❌ | O1/O2 |
| First pause at entry / TLS / system | 🟡 entry+system, ❌ TLS | O2/x64 |

**Gap:** small — "till user code", animate, TLS-callback break. Pause-while-
running has a known bug (see ROADMAP).

## 3. Breakpoints
| | odbg | field |
|---|---|---|
| Software (INT3) | ✅ `bp`, deferred mod!sym | all |
| Hardware (DR, exec/read/write) | ✅ `he`/`hr`/`hw`/`hd` | all |
| Memory breakpoints (on access/write) | ❌ | O1/O2/x64 |
| Conditional | ❌ | O1/O2/x64/WD |
| Conditional logging (log & continue) | ❌ | O1/O2/x64/WD |
| Enable/disable toggle | ❌ (remove only) | all |
| Per-thread | ❌ (engine can) | x64/WD |
| Breakpoints window | ✅ | all |

**Gap:** medium — conditional + logging + memory breakpoints are core RE
workflow; all reachable via DbgEng `SetCommandWide`/`SetDataParameters`.

## 4. Tracing
| | odbg | field |
|---|---|---|
| Hit trace (coverage) | ✅ Phase 1 | O2/x64 |
| Run trace (record every instr) | ❌ | O2/x64 |
| Trace conditions / stop-on | ❌ | O2/x64 |
| Backtrace history (+/- step back) | ❌ | O2/x64 |
| Fast emulation (Olly's 500k/s) | ❌ | O2 |
| Trace/coverage window | ❌ | O2/x64 |

**Gap: large.** This is a flagship O2 capability. DbgEng single-step caps ~5k/s
(measured); reaching usable speed needs the emulator (ROADMAP trace plan). odbg
already has the hard-won measurements and a design.

## 5. Disassembly & analysis
| | odbg | field |
|---|---|---|
| Disassembler | ✅ DbgEng (accurate, symbolized) | all |
| Syntax highlight | ✅ | O2/x64/IDA |
| Comment column | 🟡 basic | O1/O2/IDA |
| **Procedure recognition** | ❌ | IDA/O2 |
| **Loops / switches / tables** | ❌ | IDA/O2 |
| **Call-arg / stack-var naming (ARG/LOCAL)** | ❌ | O1/O2/IDA |
| **Register prediction** | ❌ | O2 |
| Cross-references (xrefs) | ❌ | IDA/G/x64 |
| Graph view (CFG) | ❌ | IDA/G/x64 |
| Known-function / API arg DB | ❌ | O1/O2/IDA |

**Gap: the largest.** Analysis is *why* people still run a frozen 32-bit Olly.
The DbgEng disassembler is fine; the recovered structure on top is absent. This
is a multi-month subsystem (ROADMAP analysis plan) — and where the symbol-first
hybrid + PDB-scored harness gives odbg an edge Olly never had.

## 6. Decompilation
| | odbg | IDA | G | x64 |
|---|---|---|---|---|
| Pseudo-C decompiler | ❌ | ✅ (Hex-Rays) | ✅ | 🟡 (snowman-ish) |

**Gap: large but optional.** Not an Olly feature at all; a "think ahead" stretch
goal. Ghidra's decompiler is the reference to study; realistically a far-future
or plugin item.

## 7. Registers & context
| | odbg | field |
|---|---|---|
| GP registers | 🟡 13 shown (no R12-R15) | all show all |
| RFLAGS with clickable flag bits | ❌ | all |
| Segment registers | ❌ | all |
| FPU / MMX | ❌ | O2/x64/WD |
| SSE / AVX (XMM/YMM) | ❌ (and `xmm` reads 0 — bug) | O2/x64/WD |
| Debug registers view | ❌ | x64/WD |
| Change highlight | ✅ | O2/x64 |
| Click-to-edit | ❌ (`reg` cmd only) | all |

**Gap: medium.** The register pane is thin. Rewriting it architecture-aware
(for 32-bit) is the moment to add R12-R15, flags, segments, FPU/SSE/AVX, and fix
the `xmm`/`st` reads-zero bug.

## 8. Memory: view / edit / map / search
| | odbg | field |
|---|---|---|
| Hex dump (byte/word/dword) | ✅ buttons | all |
| ASCII / UNICODE dump | 🟡 ascii col; ❌ dc/du modes | all |
| Float / address views | ❌ | O2/x64/WD |
| Edit memory | 🟡 `poke` bytes | all (rich editor) |
| Memory map window | ✅ | all |
| Search memory (bytes/string/pattern) | ❌ | all |
| Dump region to file | ❌ | x64/WD |

**Gap: medium.** No memory search at all (see §12). Dump/edit is minimal vs a
real hex editor + patch flow.

## 9. Patching
| | odbg | O1 | x64 |
|---|---|---|---|
| Edit bytes | ✅ `poke` | ✅ | ✅ |
| Inline assemble | ❌ (DbgEng `Assemble` unused) | ✅ | ✅ |
| Binary edit dialog | ❌ | ✅ | ✅ |
| Patch manager | ❌ | ✅ | ✅ |
| Write patch back to .exe (fixups) | ❌ | ✅ | ✅ |
| Undo | ❌ | ✅ | ✅ |

**Gap: medium.** Assemble is one DbgEng call away; patch-manager + write-back is
the OllyDump/x64dbg-parity work (ties to the dumper).

## 10. Symbols
| | odbg | field |
|---|---|---|
| Symbol server (symsrv) | ✅ | WD/x64 |
| Multiple dirs + servers, editable | ✅ | WD/x64 |
| Per-module load + progress + log | ✅ | x64/WD |
| Modules window w/ sym status | ✅ | all |
| Load a specific .pdb / for a DLL | ❌ | WD/x64/IDA |
| Custom download folder / read both | 🟡 one cache | WD/x64 |
| Source-level debugging | ❌ (engine can) | WD/x64/IDA |

**Gap: small.** Strong area. Remaining: load-specific-symbol-file, download
folder control, source debugging.

## 11. Modules / threads / handles / windows / processes
| | odbg | field |
|---|---|---|
| Modules window | ✅ w/ actions | all |
| Multi-process (child follow) tabs | ✅ | x64/WD |
| Threads window (suspend/resume/switch) | ❌ (placeholder) | all |
| Call stack window | ❌ (placeholder; engine can) | all |
| Handles window | ❌ (placeholder) | O2/x64/PH |
| Windows list | ❌ (placeholder) | O2/x64 |
| SEH / VEH chain | ❌ (placeholder) | O2/x64 |

**Gap: medium.** Shell is scaffolded; Threads and Call stack are the highest-
value real windows to fill (both backed by present DbgEng calls).

## 12. Navigation & search
| | odbg | field |
|---|---|---|
| Go to expression (Ctrl+G) | 🟡 `u <expr>` only | all |
| Follow jump/call (Enter) | ❌ | all |
| Back/forward history (+/-) | ❌ | all |
| Search command (Ctrl+F) | ❌ | O1/O2/x64/IDA |
| Search command sequence (Ctrl+S) | ❌ | O1/O2 |
| Binary/pattern search (Ctrl+B) | ❌ | all |
| Find references (Ctrl+R) | ❌ | O1/O2/IDA/G |
| Name/label list (Ctrl+N) | ❌ | O1/O2/IDA |
| String search / list | ❌ | all |

**Gap: large.** Effectively no search. Ctrl+G and binary search (`SearchVirtual`)
and follow/history are the highest-frequency keys in daily RE — cheap wins.

## 13. Annotations (remember-your-work)
| | odbg | field |
|---|---|---|
| Labels | ❌ | all |
| Comments | ❌ | all |
| Bookmarks | ❌ | O2(plugin)/x64/IDA |
| Watches | ❌ | O1/O2/x64/WD |

**Gap: medium.** None yet; pairs with persistence (§18).

## 14. Anti-anti-debug / stealth
| | odbg | field |
|---|---|---|
| PEB BeingDebugged/NtGlobalFlag | ✅ plugin | ScyllaHide |
| NtQueryInformationProcess hook | ❌ | O2/ScyllaHide |
| Non-INT3 sw breakpoints | ❌ | O2 |
| Hidden-module scan | ❌ | O2 |
| Broad ScyllaHide-class hiding | ❌ | x64+ScyllaHide |

**Gap: medium.** We have a foothold (odbg-anti_anti); the OS-hook tier is next.

## 15. Packers / unpacking / dumping
| | odbg | field |
|---|---|---|
| Run-to-OEP (HW bp survives SMC) | ✅ proven | x64/Olly+plugins |
| Generic OEP find (ESP/tail-jump) | 🟡 prototyped | plugins |
| Dump process image | ❌ | OllyDump/x64+Scylla |
| Import (IAT) reconstruction | ❌ | Scylla/ImpREC |
| Rebuild runnable PE | ❌ | Scylla |
| Packer detection | ❌ | DIE/PEiD |

**Gap: medium.** Core mechanic proven (HW bp at OEP); the dumper + IAT rebuild
(the public unpacker plugin) is the open build.

## 16. Scripting & automation — **odbg leads**
| | odbg | field |
|---|---|---|
| Command line / verbs | ✅ full | all |
| External programmatic control | ✅ named pipe (live) | WD(pipe), x64(limited) |
| Headless CLI | ✅ `odbg_cli` + JSON | WD(cdb) |
| Plugins can drive whole debugger | ✅ `Odbg_Command` | x64/IDA |
| Embedded scripting language (Python) | ✅ `odbg-python` plugin | x64/IDA/WD/G |

**Gap: closed — and we lead.** Embedded CPython (`odbg-python`: pyplugins/ +
one-shot scripts, full `Odbg_*` API incl. `Odbg_Command`) now matches
IDAPython/x64dbg-scripts, on top of the pipe + JSON CLI external control that
already beat the field.

## 17. Plugins
| | odbg | field |
|---|---|---|
| Native plugin SDK | ✅ Olly-shaped | O1/O2/x64/IDA |
| Full host API to plugins | ✅ (Odbg_Command) | x64/IDA |
| Menu/lifecycle/paused hooks | ✅ | all |
| Invoke plugin from pipe/cmd | ❌ (planned) | — |
| Ecosystem | 🟡 nascent | O1/x64/IDA huge |

**Gap: small.** Solid SDK; needs the pipe→plugin leg and, over time, an
ecosystem.

## 18. Persistence
| | odbg | field |
|---|---|---|
| Window/layout/options | ✅ | all |
| Per-module bp/labels/comments (.udd) | ❌ | O1/O2 |
| Analysis DB (.udl / .idb / project) | ❌ | O1/O2/IDA/G |
| Session/workspace save | ❌ | x64/IDA/G |

**Gap: large.** No `.udd`/project. This is the "reopen and my work is still
here" feature; ties to annotations and analysis.

## 19. Detection
| | odbg | field |
|---|---|---|
| Compiler/linker ID | ❌ | IDA/DIE |
| Packer/protector ID | ❌ | DIE/PEiD |
| Detect-and-adapt (+ manual override) | ❌ | IDA(analysis opts) |

**Gap: medium.** PEiD/DIE-style detection + adaptive analysis; think-ahead item.

## 20. UI / UX
| | odbg | field |
|---|---|---|
| CPU/Reg/Stack/Dump/Log layout | ✅ | O1/O2/x64 |
| Dockable windows | ✅ | x64 |
| Themes | 🟡 dark only | x64 |
| Rich right-click menus everywhere | 🟡 partial | O1/O2/x64 |
| Options pages (Olly-depth) | 🟡 partial | O1/O2/x64 |
| Graph view | ❌ | IDA/G/x64 |

**Gap: medium.** Shell is there; depth (right-click menus, options pages, light
theme) and the graph view are the work.

---

## Priorities that fall out of this audit

**Fill first (cheap, high-frequency, close gaps to parity):**
1. Navigation & search — Ctrl+G, binary search (`SearchVirtual`), follow/history.
2. Threads + Call stack windows (present DbgEng calls; fill the placeholders).
3. Conditional/logging/memory breakpoints (`SetCommandWide`/`SetDataParameters`).
4. Register pane rebuild → R12-R15, flags, segments, FPU/SSE/AVX, fix `xmm`=0.
5. Inline assemble (`Assemble`) + dump ASCII/UNICODE.
6. 32-bit debugging (unlocks all the x86 Olly workflows/packers).

**Big subsystems (commit deliberately):**
7. Analysis engine (procs/loops/switches/args/prediction) — the largest gap.
8. Run-trace + emulator — the O2 flagship.
9. `.udd`/project persistence + labels/comments/watches — remember-your-work.

**Finish in flight:** unpacker plugin (dump + IAT), anti-anti OS-hook tier,
pipe→plugin control, light theme, options/right-click depth.

**Keep leaning on the lead:** scriptability (pipe + CLI + Odbg_Command) and
working 64-bit — where odbg already beats the frozen Ollys.
