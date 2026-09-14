# UPX pack/unpack lab

Reference flow for open-debugger's unpacking work: pack a target with UPX, then
unpack it in odbg using a hardware breakpoint at the OEP. This is what the
unpacker plugin automates.

```
python pack_unpack_demo.py            # packs the project test target
python pack_unpack_demo.py app.exe    # or your own (non-CFG) exe
```

The script:

1. **Fetches UPX** (v5.2.1) into `_work/` the first time (gitignored).
2. **Packs** the target (`upx --best`) and verifies `upx -d` round-trips - the
   ground truth an unpacker is checked against.
3. If an **odbg** GUI is running (its `\.\pipe\odbg_cmd` is up), **drives the
   unpack**: run to the UPX stub with a software breakpoint, arm a hardware
   execute breakpoint at the OEP, continue - and stop at the decompressed OEP.

## Why a hardware breakpoint

The packed entry is the UPX stub; the real OEP sits in `UPX0`, which is empty
until the stub decompresses into it. A software `INT3` at the OEP is *overwritten*
by that decompression and never fires. A hardware execute breakpoint lives in a
debug register, touches no memory, and survives - so it fires at the unpacked
entry. (DbgEng won't arm HW breakpoints at the initial loader break, so the flow
sets it at the stub, past that point.)

## Note on targets

Use a plain PE. Win11's `notepad.exe` has Control Flow Guard and an app-execution
alias: UPX needs `--force`, and the result doesn't run. The project's own
`tools/test_target/odbg-test_target.exe` packs cleanly and runs.
