#!/usr/bin/env python3
"""t_41f38df0: look for the dead cutscene camera's float32 constants in the
game ROMFS (scripts/scene data) — DQ8.code has none, so the values are either
computed at runtime or read from romfs data.

usage: scan_romfs_for_camera.py [path]
"""
import struct
import sys

ROMFS = sys.argv[1] if len(sys.argv) > 1 else "/home/hermes/DQ8/target/dq8.romfs"

# dead cutscene camera, bw-cam5 UNICAM t=437.50 (V/M/N/A85 all consistent)
EYE = (-0.35, -17.540001, 132.75)
FWD = (-0.531412, -0.444963, 0.720839)      # = -UNI90-92 row2
VW = (78.491180, -31.929714, 103.682037)    # V translation (dead)

blob = open(ROMFS, "rb").read()
print(f"{ROMFS}: {len(blob)} bytes")


def find(pat, limit=8):
    out, start = [], 0
    while len(out) < limit:
        i = blob.find(pat, start)
        if i < 0:
            break
        out.append(i)
        start = i + 1
    return out


def hits(v, pat=None):
    p = pat if pat is not None else struct.pack("<f", v)
    return find(p, 8), len(find(p, 1 << 30))


for name, trip in (("eye", EYE), ("fwd", FWD), ("viewT", VW)):
    for v in trip:
        locs, n = hits(v)
        print(f"  {name} {v!r:24} count={n:5d} first=" +
              (",".join(f"{x:#x}" for x in locs[:6]) if locs else "-"))

print("--- clusters: >=2 distinct eye/fwd components within 12 bytes ---")
need = {}
for group in (EYE, FWD, VW):
    for v in group:
        p = struct.pack("<f", v)
        start = 0
        while True:
            i = blob.find(p, start)
            if i < 0:
                break
            need.setdefault(i // 4, []).append(v)
            start = i + 1
shown = 0
for w, vals in sorted(need.items()):
    if len(set(vals)) >= 2:
        base = w * 4
        print(f"  +{base:#010x}: {sorted(set(round(v, 6) for v in vals))}")
        shown += 1
        if shown >= 40:
            print("  ... (truncated)")
            break
if not shown:
    print("  none")
