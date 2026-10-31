#!/usr/bin/env python3
"""t_41f38df0: look for the dead cutscene camera's float32 constants in the
guest code image (DQ8.code).  If the eye/target triple is stored there, the
camera is script/data-driven and the defect is in WHICH entry is read or in the
look-at math; if it is absent, the values are computed at runtime.

usage: scan_code_for_camera.py [float ...]
"""
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
TEXT_BASE = 0x00100000

DEFAULTS = [
    # eye = UNI 25-27 translation at the dead draw (bw-cam5 UNICAM t=437.52)
    -0.35, -17.540001, 132.75,
    # forward = -UNI90-92 row2 (gluLookAt basis recovered exactly from R)
    -0.531412, -0.444963, 0.720839,
    # view translation components of V (dead)
    78.491180, -31.929714, 103.682037,
]

vals = [float(x) for x in sys.argv[1:]] or DEFAULTS
blob = open(CODE, "rb").read()
print(f"{CODE}: {len(blob)} bytes")

for v in vals:
    pat = struct.pack("<f", v)
    hits = []
    start = 0
    while True:
        i = blob.find(pat, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    if hits:
        addrs = [TEXT_BASE + h for h in hits[:12]]
        print(f"  {v!r:24} -> {len(hits)} hit(s): " +
              ", ".join(f"{a:#010x}" for a in addrs))
    else:
        print(f"  {v!r:24} -> NOT FOUND")

# neighbourhood dump around each hit of the eye triple components: are the three
# eye components adjacent (a vec3 record)?
print("--- adjacency scan: any 12-byte window holding >=2 of the eye floats ---")
pats = [struct.pack("<f", v) for v in (DEFAULTS[0], DEFAULTS[1], DEFAULTS[2])]
locs = {}
for k, p in enumerate(pats):
    start = 0
    while True:
        i = blob.find(p, start)
        if i < 0:
            break
        locs.setdefault(i // 4, []).append(k)
        start = i + 1
for w, ks in sorted(locs.items()):
    if len(ks) >= 2:
        base = w * 4
        words = struct.unpack_from("<12f", blob, base) if base + 48 <= len(blob) else ()
        print(f"  word-aligned {TEXT_BASE + base:#010x}: components {ks} " +
              (f"next floats={[round(x,4) for x in words]}" if words else ""))
