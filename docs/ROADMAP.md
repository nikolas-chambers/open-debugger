# open-debugger roadmap ("the map")

Where odbg is going and why. **Identity: this is essentially OllyDbg 3.0 x64 -
the same thing, but better.** The north star is **OllyDbg 1.10 + OllyDbg 2.0
feel and usage**, carried onto a 64-bit-capable DbgEng core (the 64-bit Olly
never shipped), with ideas mined from the best of everything since. This file is
the living plan; keep it current as features land (see the docs rule below).

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

- **Pause does not always stop a running target** - reported live; likely
  interacts with an active hit trace (worker is in the trace loop) or the async
  break-in timing. Investigate `DbgHost::BreakIn` vs. the hit-trace resume path.
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

### Breakpoints (DbgEng calls already verified present)
Conditional + logging (`SetCommandWide`), hardware (`SetDataParameters` +
`DEBUG_BREAKPOINT_DATA`), memory, per-thread (`SetMatchThreadId`), enable/
disable toggle (Olly's Space), pass-exception (`DEBUG_STATUS_GO_HANDLED`, done
as `ge`).

### Windows to add (each a DbgEng query that exists in our header)
Memory map (`QueryVirtual`), Modules (`GetModuleParameters`), Threads
(`GetNumberThreads`), Call stack (`GetStackTrace`), Breakpoints, Handles,
Windows, SEH/VEH chains, References, Watches, Patches, Run trace, a dedicated
Trace/coverage window.

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
  ubiquitous, and it has a known-correct `-d` decompress to validate our
  unpacker against. (UPX is not installed here yet; fetch it into the
  workspace.)
- Workspace: carry the packer and a packed sample in the workspace; the
  **unpacker plugin is its own public repo** (a submodule under `plugins/`,
  like `open-debugger-plugins`, not the private one).
- Plugin approach: run to OEP (generic heuristics - tail-jump/pushad-popad for
  UPX), dump the image, reconstruct imports, rebuild a runnable PE. This
  exercises and drives real odbg features (dump, memory map, OEP detection,
  IAT reconstruction) - which is the point.

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

## odbg's own differentiators (keep and lean into)

Working 64-bit (no released Olly has it), the named-pipe control channel, and
`odbg_cli` with JSON trace output - scriptability Olly only reaches via plugins.
