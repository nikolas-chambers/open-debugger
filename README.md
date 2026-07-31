# open-debugger (odbg)

A Windows user-mode debugger built on DbgEng, with an OllyDbg-shaped front end:
disassembly, registers, stack, dump, a command bar you type Olly-style verbs
into, and a log. The debug engine runs on its own thread and also serves the
same command vocabulary over a named pipe (`\\.\pipe\odbg_cmd`), so a script
can drive - and watch - the exact session a person is sitting in front of.

Two executables come out of this repo:

| target     | what it is |
|------------|------------|
| `odbg`     | the GUI debugger (Win32 + D3D11 + Dear ImGui) |
| `odbg_cli` | the same engine, scripted from the command line (`-launch`, `-break`, `-stepout`, ...) |

## Building

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`-A Win32` builds the 32-bit debugger from the same CMakeLists; both
architectures get the full engine.

Configuring downloads Microsoft's Debugging Tools DLLs (dbgeng, dbgcore,
dbghelp, dbgmodel, msdia140, symsrv, srcsrv) from their official nuget.org
packages into `third_party/bin/<arch>` - version-pinned and SHA256-checked, see
`cmake/FetchDbgEng.cmake`. They are Microsoft's binaries, so they are not
committed here. Pass `-DODBG_FETCH_DBGENG=OFF` to skip the download and supply
that folder yourself from a Windows SDK "Debugging Tools for Windows" install.

Dear ImGui and the plugin repos are submodules, so clone with:

```
git clone --recurse-submodules https://github.com/nikolas-chambers/open-debugger
```

## Plugins

`odbg.exe` exports a flat C API and loads plugin DLLs out of the `plugins`
folder next to it, the same way OllyDbg loads plugins that link against
`ollydbg.exe`. Each plugin family is its own repo, carried here as a submodule
under `plugins/`, and one configure of this project builds the debugger and
every plugin it can see:

| submodule | contents |
|-----------|----------|
| [open-debugger-plugins_sdk](https://github.com/nikolas-chambers/open-debugger-plugins_sdk) | the plugin SDK (`odbg_plugin_sdk.h`, `add_odbg_plugin()`) + `odbg-sample_plugin` |
| [open-debugger-plugins](https://github.com/nikolas-chambers/open-debugger-plugins) | `odbg-anti_anti` |
| open-debugger-plugins-private | plugins for a project of mine that is not public yet |

**The last one is a private repo.** It is registered with `update = none`, so
a normal `--recurse-submodules` clone skips it rather than failing for people
who cannot read it, and the build prints `plugins/open-debugger-plugins-private
is not checked out - skipping it` and carries on without those three plugins.
Nothing else is affected. With access, opt in:

```
git -c submodule."plugins/open-debugger-plugins-private".update=checkout \
    submodule update --init plugins/open-debugger-plugins-private
```

Writing your own plugin: the SDK repo is self-contained and documents the
whole flow - either add a folder to `open-debugger-plugins`, or carry the SDK
as a submodule in a repo of your own.

---

<table>
<tr><td>

### ☕ Buy me a coffee?

**Venmo · Cash App · PayPal — "NikAndRigatoni" (Nikolas Chambers)**

The honest version: my dog and I are living in the car right now. I spend my
days writing code anyway - bringing old projects of mine back to life one at a
time, and learning everything I can along the way. If anything here was useful
to you, a few bucks goes to dog food, gas, and keeping the laptop running, and
it buys me more hours to keep building. Either way, thanks for reading this far.

</td></tr>
</table>
