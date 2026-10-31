#!/usr/bin/env python3
# Compare DQ8_DSP_DUMP snapshots: list non-zero / changed 32-bit words per
# region of interest in the 512 KiB DSP window (0x1FF00000-based).
# Usage: python3 tools/dsp_dump_diff.py <prefix> [<prefix2> ...]
import glob, sys, struct

def load(path):
    with open(path, "rb") as f:
        return f.read()

def words(data, off, n):
    return struct.unpack_from("<%dI" % n, data, off)

def regions(d):
    # report (offset, count) runs of non-zero words in the two app regions
    out = []
    for base, name in ((0x50000, "R0"), (0x70000, "R1")):
        runs = []
        cur = None
        for off in range(0, 0x8000, 4):
            (w,) = struct.unpack_from("<I", d, base + off)
            if w != 0:
                if cur and cur[1] == off:
                    cur = (cur[0], off + 4)
                else:
                    if cur: runs.append(cur)
                    cur = (off, off + 4)
        if cur: runs.append(cur)
        out.append((name, runs))
    return out

prefixes = sys.argv[1:]
snapshots = []
for p in prefixes:
    files = sorted(glob.glob(p + "*.bin"))
    if not files:
        print("no dumps for", p)
        continue
    snapshots.append((files[0], load(files[0])))
    snapshots.append((files[len(files)//2], load(files[len(files)//2])))
    snapshots.append((files[-1], load(files[-1])))

for name, d in snapshots:
    print("=== %s (%d bytes)" % (name, len(d)))
    for rname, runs in regions(d):
        if not runs:
            print("  %s: all zero" % rname)
            continue
        print("  %s non-zero runs:" % rname)
        for lo, hi in runs:
            # summarize contents
            ws = words(d, 0x50000 if rname == "R0" else 0x70000, 0)  # unused
            base = 0x50000 if rname == "R0" else 0x70000
            vals = struct.unpack_from("<%dI" % ((hi - lo) // 4), d, base + lo)
            if len(vals) > 6:
                shown = " ".join("%08X" % v for v in vals[:6]) + " ..."
            else:
                shown = " ".join("%08X" % v for v in vals)
            print("    +0x%04X..+0x%04X (%d B): %s" % (lo, hi, hi - lo, shown))
