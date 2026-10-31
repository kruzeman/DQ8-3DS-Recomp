#!/usr/bin/env python3
"""Map exactly which guest depth pixels are zero in a dump."""
import os
import struct
import sys
from collections import Counter

XLUT = (0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15)
YLUT = (0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a)

d = sys.argv[1]
meta = open(os.path.join(d, "guest_meta.txt")).read().split()
w, h = int(meta[1]), int(meta[2])
bpp = int(meta[meta.index("depth_bpp") + 1])
raw = open(os.path.join(d, "guest_depth.raw"), "rb").read()

zeros_by_col = Counter()
rows_with_zero = set()
for row in range(h):
    for col in range(w):
        off = ((row >> 3) * (w >> 3) + (col >> 3)) * 64 + XLUT[col & 7] + YLUT[row & 7]
        i = off * bpp
        v = struct.unpack_from("<I" if bpp > 2 else "<H", raw, i)[0]
        if v == 0:
            zeros_by_col[col] += 1
            rows_with_zero.add(row)
print(f"{d}: total zeros={sum(zeros_by_col.values())} "
      f"rows={len(rows_with_zero)} (min {min(rows_with_zero) if rows_with_zero else -1}"
      f"..{max(rows_with_zero) if rows_with_zero else -1})")
print("zeros per column:", sorted(zeros_by_col.items()))
if rows_with_zero:
    rs = sorted(rows_with_zero)
    # contiguous ranges
    ranges = []
    start = prev = rs[0]
    for r in rs[1:]:
        if r == prev + 1:
            prev = r
        else:
            ranges.append((start, prev))
            start = prev = r
    ranges.append((start, prev))
    print("row ranges:", ranges)
