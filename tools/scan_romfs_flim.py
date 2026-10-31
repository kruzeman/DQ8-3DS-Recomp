#!/usr/bin/env python3
"""Scan the DQ8 RomFS blob for BFLIM footers and report format/size/dim."""
import struct, sys
from collections import Counter

path = sys.argv[1] if len(sys.argv) > 1 else "target/dq8.romfs"
data = open(path, "rb").read()
print("romfs size", len(data))

hits = []
pos = 0
while True:
    i = data.find(b"FLIM", pos)
    if i < 0:
        break
    if i + 40 > len(data):
        pos = i + 4
        continue
    endian = "<" if data[i + 4:i + 6] == b"\xff\xfe" else ">"
    w, h, _align = struct.unpack_from(endian + "HHH", data, i + 28)
    fmt = data[i + 34]
    tiles = data[i + 35]
    rsize = struct.unpack_from(endian + "I", data, i + 36)[0]
    hits.append((i, fmt, w, h, rsize))
    pos = i + 4

print("total FLIM", len(hits))
print("fmt distribution:", dict(Counter(h[1] for h in hits)))
for tag in (10, 11, 12, 13):
    ex = [h for h in hits if h[1] == tag]
    if ex:
        e = ex[0]
        print(f"fmt={tag} count={len(ex)} e.g. @{e[0]} w={e[2]} h={e[3]} rawsize={e[4]}")
