#!/usr/bin/env python3
"""Presented-frame identity differential between two DQ8 runner binaries.

Both runners execute the SAME deterministic fixed-work segment
(DQ8_BENCH_STEPS, the tools/bench.sh budget) from the SAME user-data seed; the
segment ends in bench_report(), which calls exit(0).  gdb stops the live
process on libc exit and writes out the two presented screen shadows
(host_shadow_top 400x240x4, host_shadow_bot 320x240x4); identical sha256 means
the same picture was on screen.

No source change and no rebuild: the shadows are resolved through gdb's
minimal-symbol table (-g0 leaves them untyped) and dumped as raw bytes.  Useful
for an A/B of any runtime change (compile flags, renderer, capture) that must
not alter what the guest displays.

usage: tools/frame_dump_diff.py <steps> <outdir> <seed-userdata|-> <bin> [<bin>...]

Caveat (audit PERF-02): `steps` counts main-context instructions only, so a
budget far from the start does not pin the endpoint - worker slices and the
host-paced producers (vblank scan-out, audio ticks) differ run to run.  Keep
the budget inside the deterministic window, or use the same-binary control run
to show what the harness alone can vary.
"""
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOP = (400, 240)
BOT = (320, 240)


def dump_script(steps, userdata, outdir, tag):
    return f"""set pagination off
set confirm off
set height 0
set environment DQ8_HEADLESS 1
set environment DQ8_RUN_GUEST_THREADS 1
set environment DQ8_BENCH_STEPS {steps}
set environment DQ8_BENCH_NO_SNAPSHOT 1
set environment DQ8_USER_DATA_DIR {userdata}
break exit
run
python
import gdb, re
def dump(sym, size, path):
    info = gdb.execute("info address " + sym, to_string=True)
    addr = int(re.search(r'0x[0-9a-fA-F]+', info).group(0), 16)
    gdb.execute("dump binary memory %s 0x%x 0x%x" % (path, addr, addr + size))
dump("host_shadow_top", {TOP[0] * TOP[1] * 4}, "{outdir}/{tag}-top.rgba")
dump("host_shadow_bot", {BOT[0] * BOT[1] * 4}, "{outdir}/{tag}-bot.rgba")
end
kill
quit
"""


def sha(p):
    p = Path(p)
    return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None


def main():
    steps, outdir, seed, *bins = sys.argv[1:]
    outdir = Path(outdir).resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    for b in bins:
        binp = Path(b).resolve()
        tag = binp.name
        ud = outdir / f"userdata-{tag}"
        if ud.exists():
            shutil.rmtree(ud)
        if seed != "-":
            shutil.copytree(seed, ud)
        cmd = outdir / f"{tag}.gdb"
        cmd.write_text(dump_script(steps, ud, outdir, tag))
        with open(outdir / f"{tag}.gdb.log", "w") as fh:
            rc = subprocess.run(["gdb", "-q", "-batch", "-x", str(cmd),
                                 "--args", str(binp), "DQ8.code",
                                 "target/dq8.romfs"],
                                cwd=ROOT, stdout=fh, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL).returncode
        print(f"{tag:32s} gdb rc={rc}")
    print("== presented-frame sha256")
    for b in bins:
        tag = Path(b).name
        print(f"{tag:32s} top={sha(outdir / f'{tag}-top.rgba')} "
              f"bot={sha(outdir / f'{tag}-bot.rgba')}")
    if len(bins) == 2:
        t = [Path(b).name for b in bins]
        same = all(sha(outdir / f"{t[0]}-{k}.rgba") is not None and
                   sha(outdir / f"{t[0]}-{k}.rgba") == sha(outdir / f"{t[1]}-{k}.rgba")
                   for k in ("top", "bot"))
        print("FRAME IDENTITY:", "IDENTICAL" if same else "DIFFERENT")
        return 0 if same else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
