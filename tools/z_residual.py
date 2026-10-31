#!/usr/bin/env python3
"""Row-by-row residual difference between two guest depth dumps (all rows)."""
import os
import struct
import sys

XLUT = (0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15)
YLUT = (0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a)


def load(d):
    meta = open(os.path.join(d, "guest_meta.txt")).read().split()
    rt_w, rt_h = int(meta[1]), int(meta[2])
    bpp = int(meta[meta.index("depth_bpp") + 1])
    return rt_w, rt_h, bpp, open(os.path.join(d, "guest_depth.raw"), "rb").read()


def z(raw, w, bpp, row, col):
    off = ((row >> 3) * (w >> 3) + (col >> 3)) * 64 + XLUT[col & 7] + YLUT[row & 7]
    i = off * bpp
    if i + bpp > len(raw):
        return None
    return struct.unpack_from("<I" if bpp > 2 else "<H", raw, i)[0] & ((1 << (8 * bpp)) - 1)


wa, ha, ba, ra = load(sys.argv[1])
wb, hb, bb, rb = load(sys.argv[2])
rows = []
for row in range(ha):
    diff = za = zb = 0
    cols = []
    for col in range(wa):
        x = z(ra, wa, ba, row, col)
        y = z(rb, wb, bb, row, col)
        if x is None or y is None:
            continue
        za += (x == 0)
        zb += (y == 0)
        if x != y:
            diff += 1
            cols.append(col)
    if diff or za or zb:
        rows.append((row, diff, za, zb, cols[:6]))
print(f"rows with any diff/zero: {len(rows)}")
for row, diff, za, zb, cols in rows[:40]:
    print(f"  row {row:3d}: diff={diff:3d} A_zeros={za:3d} B_zeros={zb:3d} first_cols={cols}")
if len(rows) > 40:
    print(f"  ... ({len(rows) - 40} more rows)")
tot_diff = sum(r[1] for r in rows)
tot_za = sum(r[2] for r in rows)
tot_zb = sum(r[3] for r in rows)
print(f"totals: diff={tot_diff} A_zeros={tot_za} B_zeros={tot_zb}")
