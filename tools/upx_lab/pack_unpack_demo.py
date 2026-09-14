#!/usr/bin/env python3
# UPX pack/unpack lab for open-debugger.
#
# End to end: fetch UPX (once), pack a target, verify the round-trip, then -
# if an odbg GUI is running with its command pipe - drive the actual unpack in
# the debugger using a hardware breakpoint at the OEP. This is the reference
# flow the unpacker plugin automates.
#
#   python pack_unpack_demo.py [target.exe]
#
# With no target it packs the project's own test target (which packs cleanly;
# Win11 notepad.exe has CFG + an app-execution-alias and is a poor first target).
#
# The debugger step is skipped automatically if odbg's pipe is not up; the
# pack/verify part always runs. UPX and the packed samples live under _work/
# (gitignored) - nothing third-party or generated is committed.

import os, sys, struct, time, zipfile, urllib.request, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, '_work')
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
DEFAULT_TARGET = os.path.join(REPO, 'tools', 'test_target', 'odbg-test_target.exe')
UPX_VER = '5.2.1'
UPX_URL = f'https://github.com/upx/upx/releases/download/v{UPX_VER}/upx-{UPX_VER}-win64.zip'

sys.path.insert(0, os.path.join(REPO, 'tools'))


def log(msg): print(msg, flush=True)


def ensure_upx():
    exe = os.path.join(WORK, f'upx-{UPX_VER}-win64', 'upx.exe')
    if os.path.exists(exe):
        return exe
    os.makedirs(WORK, exist_ok=True)
    zpath = os.path.join(WORK, 'upx.zip')
    log(f'fetching UPX {UPX_VER} ...')
    urllib.request.urlretrieve(UPX_URL, zpath)
    with zipfile.ZipFile(zpath) as z:
        z.extractall(WORK)
    log(f'  -> {exe}')
    return exe


def pe_entry(path):
    """Return (AddressOfEntryPoint RVA, ImageBase) of a PE file."""
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    assert d[pe:pe+4] == b'PE\x00\x00', 'not a PE'
    ep = struct.unpack_from('<I', d, pe + 24 + 16)[0]
    magic = struct.unpack_from('<H', d, pe + 24)[0]
    base = struct.unpack_from('<Q', d, pe + 24 + 24)[0] if magic == 0x20b \
        else struct.unpack_from('<I', d, pe + 24 + 28)[0]
    return ep, base


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def pipe_up():
    try:
        import odbg_pipe
        r = odbg_pipe.send('exceptions', retries=1)
        return not r.startswith('(fail')
    except Exception:
        return False


def drive_unpack(packed, oep_rva, stub_rva):
    import odbg_pipe
    send = odbg_pipe.send

    def ripv():
        r = send('reg rip')
        try: return int(r.split('=')[1].strip(), 16)
        except Exception: return -1

    def wait_at(addr, t=10):
        t0 = time.time()
        while time.time() - t0 < t:
            if ripv() == addr: return True
            time.sleep(0.03)
        return False

    log('\n--- driving the unpack in odbg ---')
    send('kill'); time.sleep(0.3)
    log('launch  -> ' + send('launch ' + packed))
    for _ in range(60):
        time.sleep(0.08)
        if send('reg rip').startswith('rip = 0x7'): break

    # image base via PEB.ImageBaseAddress (symbol-independent; @$exentry needs
    # symbols the target may not have).
    r = send('eval poi(@$peb+0x10)')
    base = int(r.split('=')[1].strip().split()[0], 16)
    stub, oep = base + stub_rva, base + oep_rva
    log(f'base 0x{base:x}  stub 0x{stub:x}  OEP 0x{oep:x}')

    # 1) run to the UPX stub with a software bp (UPX1 code, never overwritten).
    send('bp %x' % stub); send('g')
    if not wait_at(stub):
        log('!! never reached the stub'); return False
    send('bc 0')

    # 2) hardware execute bp at the OEP - survives the stub decompressing over
    #    it (a software INT3 there gets clobbered). NB: HW bps do not arm at the
    #    initial loader break, so we set it here, past that, at the stub.
    log('he OEP  -> ' + send('he %x' % oep))
    send('g')
    if wait_at(oep, 12):
        log('RESULT  -> HW bp fired at the unpacked OEP. Image is decompressed.')
        send('u %x' % oep)   # point the CPU view at the now-real code
        return True
    log('RESULT  -> did not reach OEP'); return False


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_TARGET
    if not os.path.exists(target):
        log(f'target not found: {target}'); return 2

    upx = ensure_upx()
    os.makedirs(WORK, exist_ok=True)
    packed = os.path.join(WORK, os.path.splitext(os.path.basename(target))[0] + '_upx.exe')
    import shutil; shutil.copy(target, packed)

    log(f'\npacking {os.path.basename(target)} ...')
    r = run([upx, '--best', packed])
    if r.returncode != 0:
        log(r.stdout + r.stderr)
        log('pack failed (CFG-guarded exe? try a non-CFG target)'); return 1
    log('  ' + [l for l in r.stdout.splitlines() if 'win64/pe' in l][-1].strip())

    # ground truth: upx -d round-trips.
    rt = os.path.join(WORK, 'roundtrip.exe'); shutil.copy(packed, rt)
    run([upx, '-d', rt])
    same_size = os.path.getsize(rt) == os.path.getsize(target)
    log(f'  upx -d round-trip size matches original: {same_size}')

    oep_rva, _ = pe_entry(target)
    stub_rva, _ = pe_entry(packed)
    log(f'  OEP rva=0x{oep_rva:x}   packed stub rva=0x{stub_rva:x}')

    if pipe_up():
        drive_unpack(packed, oep_rva, stub_rva)
    else:
        log('\n(odbg pipe not up - start build/Release/odbg.exe to run the '
            'debugger unpack step)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
