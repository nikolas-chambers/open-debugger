# open-debugger roadmap ("the map")

Where odbg is going and why. **Identity: this is essentially OllyDbg 3.0 x64 -
the same thing, but better.** The north star is **OllyDbg 1.10 + OllyDbg 2.0
feel and usage**, carried onto a 64-bit-capable DbgEng core (the 64-bit Olly
never shipped), with ideas mined from the best of everything since. This file is
the living plan; keep it current as features land (see the docs rule below).

**Fixing OllyDbg to work right is half the point.** Not just re-skinning Olly -
correcting where Olly was flaky or wrong (unreliable hit trace on real programs,
HW-breakpoint edge cases, analysis errors, missing 64-bit) and doing it
properly. "Same but better" means better.

The plugin SDK grows as we go - when a feature needs a new host export or
callback, add it to the SDK (and its docs) in the same pass.

## Non-negotiable design principles

1. **Everything is controllable from the GUI, the pipe/command line, AND
   plugins** - the three-way control triangle. Every feature ships with a GUI
   control *and* a verb, from day one.
   - Plugins -> everything: **done** via `Odbg_Command(cmdline, out, outSize)`
     in the SDK - it runs any verb through the same dispatcher the GUI and pipe
     use, so a plugin has the whole surface and auto-gains new verbs.
   - Pipe/GUI -> plugins: **next** - a plugin's menu items and registered
     commands must be invocable over the pipe and from the command bar (e.g. a
     `plugin list` / `plugin fire <name> <action>` verb family), not only from
     the Plugins menu. This closes the triangle.
   The named-pipe channel is a first-class interface, not an afterthought.
2. **Options everywhere, and better than Olly/x64dbg.** Rich, well-organized
   options pages; highlighting/appearance options; every basic that Olly and
   x64dbg expose, plus ones that genuinely benefit this debugger. Study their
   options dialogs and beat them.
3. **Rich right-click context menus** on every view (disasm, dump, stack,
   registers) - Olly's are the bar. Follow address/jump/call targets, set
   things, copy, edit, all from the line under the cursor.
4. **Docs move with the code.** Any change to a verb, plugin export, or build
   step updates CONTROL.md / README.md / the SDK docs in the same commit.
5. **Commit and push each completed feature** as it lands, attributed to
   nikolas-chambers only.

## Known bugs (fix next, each its own commit)

- **Pause does not always stop a running target** - reported live and
  **reproduced**: during an active hit trace, `pause` returns "break requested"
  but the target keeps running (rip reads 0). The worker is busy in the
  trace loop (PumpOneEvent -> hit bp -> Go) and the injected break races the
  next hit-trace resume. Investigate `DbgHost::BreakIn` vs. the hit-trace path.
- **"Follow in Dump" context item does not work**, and right-clicking an
  instruction should offer to follow any address it references (jump/call
  target, memory operand) into the disasm or dump. Wire the follow actions and
  add operand-address extraction.

## Verbs

Done (this is the contract - see CONTROL.md): `reg` (r), `poke` (eb),
`eval`/`?`/`calc`, `si`/`s`, `so` (p), `tr` (rtr), `ge`, `run` (g), `pause`/
`stop`, `at`/`follow`/`u`, `orig`/`*`, `ht`/`htclear`, plus odbg extensions
(`launch`, `attach`, `kill`, `restart`, `bp`, `bc`, `d`/`db`/`dw`/`dd`,
`childdbg`, `breakmod`, `breakthread`, `ignoreexc`/`catchexc`/`rmexc`/
`exceptions`, `proc`/`closeproc`). `t` is reserved for run-trace.

Naming rule: **more verbs than Olly is fine** - just name them so they read
obviously (no cryptic verbs). Olly-compatible where Olly has one; clear odbg
names otherwise.

Planned verbs (need the feature first): `a` (assemble), `l`/`:` (label), `c`
(comment), `w`/`watch`, `mr`/`mw`/`md` (memory bp), `hr`/`hw`/`he`/`hd`
(hardware bp), `t`/`ti`/`to`/`tc`/`toc` (run trace), `tu` (till user code),
window-focus verbs (`cpu`/`log`/`mem`/`mod`/`cs`/`brk`/`opt`).

## Feature phases

### Done
- **Phase 1 - hit trace.** Full-speed coverage discovery via dynamically armed
  branch breakpoints. Trace menu, toolbar toggle, green coverage margin.

### Trace engine (the headline 2.0 capability)
Measured ceilings on this stack (see `tools/stepbench` when added): DbgEng
single-step ~5,400/sec; the debug-event round trip (~185us) is irreducible;
raw-handle bypass is *slower*, not faster (engine caches register/memory reads);
recording is free. Therefore:
- **Run trace on real stepping** first - record format, backtracking UI
  (Olly's `+`/`-` through history), trace conditions. Everything but speed.
- **Condition compiler** - compile conditions to pseudocode (Olly hits ~130ns
  per eval); needed before fast trace is usable.
- **Emulator** - the only path to Olly's 500k/sec. Emulate user code; **escape
  by running past whole calls to a return breakpoint** rather than stepping the
  callee (our departure from Olly - one 444us escape covers an entire API call
  instead of thousands of steps). Self-modifying-code guard included.

### Analysis (what makes Olly readable)
Symbol-first hybrid: **use the PDB when present, fall back to Olly's heuristic
analyser when not** (the primary case - stripped/packed/hostile binaries).
- Ranked symbol resolver first (PDB > exports > user labels > imported DB >
  analysis), each annotation tagged with provenance so inferred names render
  differently from known ones. (The declared-but-unimplemented `LoadSymbolMap`/
  `LoadSdk` overlay in dbghost.h is the seam.)
- Scoring harness against system-DLL PDBs (public symbols we already fetch) -
  Olly never had a ground-truth oracle; we do.
- Then Olly's passes: call-count entry discovery (>=3 calls = entry), recursive
  code walk, loop detection, ESP tracking -> ARG.n/LOCAL.n, switch vs.
  cascaded-if, register prediction. Known-function DB (Olly ships 2200+ APIs,
  7800+ constants).

**Per-module analysis on load, Olly-style.** For each DLL as it loads: kick off
symbol search/download (symbol server) and analysis, with a **progress bar**,
and a prompt (an options-text line) letting the user **press Space to skip
analyzing that module** (big system DLLs are often not worth analyzing). Same
feel as Olly analyzing a module on load, but non-blocking and skippable.

### Breakpoints (DbgEng calls already verified present)
- **Hardware breakpoints: done** - `he`/`hr`/`hw`/`hd` (`SetDataParameters` +
  `DEBUG_BREAKPOINT_DATA`). Survive self-modifying code / unpackers. Note the
  DbgEng quirk (documented in CONTROL.md): they do not arm at the initial
  loader break, only once the target is running.
- Pass-exception (`DEBUG_STATUS_GO_HANDLED`): done as `ge`.
- Still to do: conditional + logging (`SetCommandWide`), memory breakpoints,
  per-thread (`SetMatchThreadId`), enable/disable toggle (Olly's Space).

### Windows to add (each a DbgEng query that exists in our header)
Memory map (`QueryVirtual`), Modules (`GetModuleParameters`), Threads
(`GetNumberThreads`), Call stack (`GetStackTrace`), Breakpoints, Handles,
Windows, SEH/VEH chains, References, Watches, Patches, Run trace, a dedicated
Trace/coverage window.

### Symbols & modules (more)
Done: configurable multi-entry symbol path (dirs + servers), per-module symbol
loading with a progress bar and per-module `[sym]` log lines, a Modules window
(Alt+E) with per-module actions. Still to do:
- **Symbol download folder control**: choose where downloads cache (our folder
  vs. the Windows-standard store), with an option to read from both.
- **Load a specific symbol file**: a file chooser / give a `.pdb` or DLL path to
  load symbols for one module (pairs with the Modules window "Load symbols").
- **Per-module analysis with "press Space to skip"** (Olly-style): as each
  module's symbols/analysis load, allow skipping big system DLLs.
- **`.udd` / `.udl` persistence** (OllyDbg): per-module save of breakpoints,
  labels, comments, watches and analysis, restored next session. odbg has none
  of this yet - the biggest missing "remembers your work" feature.

### Source & SDK integration (debug your own project)
Realistic difficulty tiers - the easy parts are safe and worth doing; the hard
parts are genuinely risky, so scope them honestly.
- **Source-level debugging of your own build (EASY, safe).** Given a PDB with
  line info + the source tree, DbgEng already maps addresses to source lines
  (`GetLineByOffset`, source path / srcsrv). This is the low-risk win: point odbg
  at your project's `.pdb` and source dir and step in source. Add a source view
  and a source path setting (like the symbol path). No parsing required - the
  compiler already put the types and lines in the PDB.
- **`-map` / `-sdk` overlays (MEDIUM, already stubbed).** Implement the
  declared `LoadSymbolMap` (text `module rva name [size]`) and `LoadSdk`
  (interface/vtable method DB) in dbghost.h - name addresses and vtable slots
  from a supplied file. Safe, bounded, and useful for naming without a PDB.
- **Ingesting headers / C++ types without a PDB (HARD, risky).** Parsing
  arbitrary C/C++ headers for struct/type info needs a real front end
  (libclang) - fragile, heavy, and easy to get subtly wrong (ABI, packing,
  templates). Treat as a large, optional, later item; prefer "build your target
  with a PDB" which gives the types for free. Pulling raw `.cpp`/headers as
  *display* (not as a type oracle) is just source-path mapping and is safe.

Net: source-level debugging of a project you build yourself is achievable and
low-risk; deriving types from headers of something you *cannot* rebuild is the
buggy/dangerous part - roadmap it, don't rush it.

### Decompiler (C/C++ pseudocode)
Seen in IDA (Hex-Rays), Ghidra, Binary Ninja, x64dbg (Snowman-class). Writing
one is a massive subsystem (Ghidra/Hex-Rays are years of work), so realistic
options, cheapest first:
- **Integrate an existing open decompiler** - e.g. drive Ghidra's decompiler
  headless, or a library, from a plugin, and show the C/C++ in a pane. Mind the
  licenses (Ghidra is Apache-2 actually; Hex-Rays is commercial).
- **Depends on analysis first** - a decompiler needs the CFG / procedure / type
  recovery from the analysis engine; without §Analysis there is nothing to
  decompile. So this trails the analysis subsystem regardless.
- **Own decompiler: far-future / stretch**, not an Olly feature at all. Keep it
  on the horizon as a "think ahead" goal, likely a plugin.

### Detection (compiler / packer / code type)
OllyDbg recognizes code shapes; go further, PEiD / Detect-It-Easy style:
identify the **compiler / linker / packer / protector** from signatures and
heuristics, and surface it. Then **detect-and-adapt** - adjust analysis and
options based on what was detected - always with **manual overrides** so the
user can force a choice. Everything informative, logged like Olly.

### Dump formats (more)
Done: Hex / Word / Dword buttons. Add **ASCII** and **UNICODE** right-side
interpretations (OllyDbg's `dc` / `du`), and float/address views, as further
format buttons routed through dump commands.

### Resource viewer
Olly's "View resources" / resource strings. Parse the PE resource directory of
the loaded module and browse it - strings, dialogs, menus, version info,
manifests, icons/bitmaps. GUI viewer + pipe verb to dump a resource.

### Dumper / editor
Dump and edit the debuggee - a first-class feature and the backbone of the
unpacker milestone:
- **Process/image dumper** - dump a module or region to disk, rebuild a
  runnable PE (fix headers/sections), pair with IAT reconstruction (Scylla
  class). This is what the UPX unpack exercise drives.
- **Memory/hex editor** - richer than the current `poke`: a real hex editor
  view (edit as bytes/ASCII/UNICODE, Olly's Ctrl+E), plus binary edit in the
  disasm and dump. Patch manager that writes changes back to the on-disk exe
  with fixups (Olly's copy-to-executable).
Likely delivered partly in-core and partly as a plugin; expose all of it over
the pipe too.

### UI / usage fidelity
- Full Olly keymap (F2/F4/Ctrl+F9/Alt+F9/Ctrl+G/Ctrl+F/S/N/R/A/K/B, Space,
  `:`, `;`, Enter-follow, `+`/`-` history, `*`, Alt+C/L/M/E/B).
- Menu structure matched to Olly 2 (File/View/Debug/Trace/Options/Window/Help).
  Trace menu exists.
- Four-pane CPU window layout, disassembler comment column + procedure-bracket
  margin (`$`/`>`/`.` markers).
- **32-bit debugging.** We build both `-A x64` and `-A Win32`, but the register
  model is x64-hardcoded (`RegFile`), so 32-bit targets read zero. OllyDbg 2 is
  32-bit-only, so this is required for real Olly-workflow parity. Rewrite the
  register model architecture-aware (also fixes the `xmm`/`st` return-zero bug
  and adds EFLAGS-with-flag-bits, segments, FPU/SSE, click-to-edit).
- **Light theme** (default stays dark). Current themes are all dark
  (Classic Olly / Dark Modern / Solarized Dark).

## Must-have plugins/features to match (Olly 1.10 / 2.0 + x64dbg ecosystem)

Target parity with (and bundling analogues of) the plugins reversers actually
rely on:
- **Unpackers / OEP finders** - OllyDump / OllyDumpEx, our own generic unpacker
  (see below).
- **Anti-anti-debug** - StrongOD / ScyllaHide / PhantOm class. We already have
  `odbg-anti_anti`; extend toward NtQueryInformationProcess interception,
  non-INT3 software breakpoints (INT1/HLT/CLI/STI), PEB patching, hidden-module
  scan.
- **Import reconstruction** - Scylla / ImpREC class (essential companion to
  unpacking).
- **API/call tracing & logging** (Olly 2's traceapi sample).
- **Command-line/scripting plugin** - largely subsumed by our pipe, but keep
  script-file execution in mind.
- **Bookmarks, comments/labels sharing, analysis helpers.**

## The packer/unpacker exercise (milestone)

Goal: take a common, source-available packer, pack `notepad.exe`, and build a
**public** unpacker plugin comparable to top-end unpacker plugins - and unpack
it - while expanding odbg's functionality so the flow is as smooth as Olly's.

- **Packer: UPX** - the obvious easy/common first target: open source (C++),
  ubiquitous, and it has a known-correct `-d` decompress to validate against.
  UPX 5.2.1 fetched; a fetch+pack+unpack demo lives in `tools/upx_lab/`. Pack
  our own test target, not notepad (Win11 notepad has CFG + app-alias and needs
  `--force`, then does not run). `upx --best odbg-test_target.exe` packs cleanly
  (151K -> 76K) and runs; `upx -d` round-trips (ground truth).
- **Reaching the OEP is proven** (drove it over the pipe): the packed exe's
  entry is the UPX stub (UPX0 empty, UPX1 = compressed + stub). A *software*
  bp at the OEP is clobbered when the stub decompresses over it; a **hardware
  execute bp at the OEP survives** and fires at the unpacked entry. Flow: run
  to the stub with `bp`, `he <OEP>`, `g` -> stop at the decompressed OEP. Hit
  trace also traces straight through the unpacking (lazy arming), a nice cross-
  check.
- **Dump the unpacked image** - "dump unpacked" option (in OllyDbg this was a
  separate plugin, OllyDump / OllyDumpEx). Read the decompressed image from
  memory, fix section headers (raw = virtual), set entry = OEP, then
  reconstruct imports (Scylla class) and rebuild a runnable PE. Likely a core
  `dump` verb/GUI action the plugin calls, plus IAT reconstruction.
- Deliverables from this exercise: **(1) fixes/features in odbg core** (HW
  breakpoints done; dumper + IAT next), **(2) a public unpacker plugin** (its
  own repo, submodule under `plugins/`, with great options), and **(3) the
  `tools/upx_lab/` demo pack/unpack script.**
- Make it **awesome with great options**: choice of OEP-find strategy (HW-bp
  tail / known offset / hit-trace), auto-dump, import reconstruction on/off,
  and a one-click "unpack this" that runs the whole flow.

## Reference sources to mine (ideas, mind the licenses)

- **OllyDbg 1.10 disassembler source** - available; useful for the disasm/
  analysis engine.
- **OllyDbg 64 (odbg64)** - never released, incomplete; scrape ideas for the
  64-bit direction Olly was heading.
- **x64dbg** - the modern open-source Olly-like (x86+x64, huge plugin
  ecosystem). "Beat us to it" - so it's an **ideas and plugin-ecosystem
  reference, not a code source**: GPLv3, and Qt + its own engine rather than
  DbgEng. Study its UX, options, plugin API surface.
- **IDA** - navigation, xrefs, naming, graph view, type system ideas.
- **Ghidra** - yes, it has a full Swing GUI (CodeBrowser + live decompiler) and
  a headless mode; its **decompiler** is the standout to study for any
  higher-level view.

## Strategy: orchestrate the best free tools (don't reinvent)

odbg is a fast, live debugger **core** (DbgEng + our UI + pipe + embedded
Python) that **conducts a toolbox of best-of-breed free tools as plugins**,
rather than rebuilding each. This is how the strongest free stacks are built
(rizin embeds Ghidra's decompiler; x64dbg leans on Scylla/ScyllaHide). Two
integration surfaces already exist: **Python plugins** (`pip install` any lib)
and **shell-out + the pipe** (drive a CLI tool). Rule: **detect-and-use** (like
symsrv) - a tool plugin lights up if its tool is present (or offers to fetch
it), else stays dormant, keeping the core lean.

The toolbox (each a plugin):
- **Unicorn Engine** (pip) -> CPU emulator for the **run-trace** engine. **Done
  as a proof-of-concept** (`emu` command): emulates from RIP, lazily copying
  live-process pages into Unicorn, seeded from the debuggee context - verified
  faithful on notepad. Turns the biggest "large subsystem" (a trace emulator)
  into an integration. Next: escape to the live process for syscalls/APIs, then
  the record/back-step UI.
- **Ghidra** (headless, shell-out) -> **decompiler** pane (`decompile` command).
  The decompiler gap without writing one (Apache-2.0). Detect-and-use; needs a
  JDK 21 for Ghidra 12.
- **Zydis** (core) / **Capstone** + **Keystone** (Python) -> disasm/asm.
- **Detect-It-Easy** (`diec`) -> packer/compiler detection.
- **Scylla** -> dump + IAT reconstruction (the unpacker milestone's hard half).
- **YARA** (pip) -> memory pattern/rule scanning.
- **pefile / LIEF** (pip) -> PE parsing/rebuild.
- **angr** (pip, opt-in) -> symbolic execution.
- **ScyllaHide** -> anti-anti-debug OS-hook tier.

## AI copilot (agent with hands on a live process)

The differentiator: odbg already exposes the **whole debugger as a clean tool
API** (pipe + `Odbg_Command` + the Python SDK) - exactly what an LLM agent needs.
So we don't bolt an LLM onto a static listing (Gepetto-style); we build an **AI
agent that drives the live debugger** - sets breakpoints, runs, steps, reads
memory - and reasons over dynamic state, using the same commands a human types
(transparent; you can take over).
- Build on **odbg-python**: an agent **tool-use loop** whose tools map to
  SDK/debugger ops **and the integrated toolbox** ("decompile this" -> Ghidra,
  "emulate this" -> Unicorn, "what packer?" -> DIE, "scan for X" -> YARA,
  "find an input reaching here" -> angr). The AI conducts the orchestra.
- **MVP**: an `ai <question>` verb (via the `register_verb` hook just added) -
  gather context, call the model, stream to the Log; grow into the tool-loop,
  then a chat window.
- **Local-model-first** (Ollama / llama.cpp) for malware privacy; cloud
  (Claude API, etc.) as an opt-in power mode; a setting picks the backend.
- **Safety**: propose-then-human-approves for execution on live/hostile targets.

## Engine strategy: DbgEng vs. rolling our own

Question raised: rewrite dbgeng.dll better? **Decision: no wholesale rewrite -
selective bypass instead.** A full rewrite means rebuilding symbols, PDB /
symbol-server, the disassembler and expression evaluator from scratch - the
x64dbg-scale, multi-year effort (they leaned on DbgHelp + Zydis + TitanEngine
rather than write it all). Not worth it.

Instead, **own the hot paths where DbgEng is slow or in the way**, keep it for
what it is good at:
- Keep DbgEng (or DbgHelp + a disassembler like Zydis) for symbols, PDB /
  symbol server, disassembly, expression eval.
- Take direct control of the **debug-event loop and thread-context / memory
  access** (WaitForDebugEvent / ContinueDebugEvent, Get/SetThreadContext,
  Read/WriteProcessMemory) for the trace emulator (the ~185us DbgEng round trip
  is the whole reason the emulator exists), for stealth/anti-anti breakpoints,
  and for precise control DbgEng makes awkward (e.g. HW breakpoints at the
  initial break). This is already implied by the trace-engine plan.

Measured caveat: raw single reads while stopped are *slower* than DbgEng's
cached ones - the win from going direct is in the tight emulator loop (local
memory cache, no per-instruction round trip), not in one-off reads.

## odbg's own differentiators (keep and lean into)

Working 64-bit (no released Olly has it), the named-pipe control channel, and
`odbg_cli` with JSON trace output - scriptability Olly only reaches via plugins.
