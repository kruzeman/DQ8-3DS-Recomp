#!/usr/bin/env python3
"""Depth forensics on the flush dump: per-GL-row histogram of FBO depth vs
guest depth, focused on where the dialog failed to render."""
import os
import struct
import sys

d = sys.argv[1]
with open(os.path.join(d, "z_fbo_last.box")) as f:
    x0, y0, x1, y1, rt_w, rt_h = (int(v) for v in f.read().split())
bw, bh = x1 - x0 + 1, y1 - y0 + 1
zbuf = open(os.path.join(d, "z_fbo_last.raw"), "rb").read()
zg = open(os.path.join(d, "z_guest_last.raw"), "rb").read()

XLUT = (0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15)
YLUT = (0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a)


def z_fbo(x, y):
    i = ((y - y0) * bw + (x - x0)) * 4
    if i < 0 or i + 4 > len(zbuf):
        return None
    return struct.unpack_from("<f", zbuf, i)[0]


def z_guest(row, col):
    off = ((row >> 3) * (rt_w >> 3) + (col >> 3)) * 64 + XLUT[col & 7] + YLUT[row & 7]
    i = off * 4
    if i + 4 > len(zg):
        return None
    return struct.unpack_from("<I", zg, i)[0]


print("rows: GL y | guest row | n | z_fbo==z_guest | distinct z_fbo (top5)")
for y in range(340, 400, 2):
    n = eq = 0
    hist = {}
    histg = {}
    for x in range(x0, x1 + 1):
        zf = z_fbo(x, y)
        zg_v = z_guest(rt_h - 1 - y, x)
        if zf is None or zg_v is None:
            continue
        n += 1
        if int(zf + 0.5) == zg_v:
            eq += 1
        hist[int(zf)] = hist.get(int(zf), 0) + 1
        histg[zg_v] = histg.get(zg_v, 0) + 1
    top = sorted(hist.items(), key=lambda kv: -kv[1])[:4]
    topg = sorted(histg.items(), key=lambda kv: -kv[1])[:4]
    print(f"  y={y:3d} row={rt_h - 1 - y:3d} n={n:3d} eq={eq * 100.0 / max(n, 1):5.1f}% "
          f"fbo={top} guest={topg}")

# global zero-depth coverage
zeros_fbo = zeros_guest = tot = 0
for y in range(y0, y1 + 1):
    for x in range(x0, x1 + 1):
        zf = z_fbo(x, y)
        zg_v = z_guest(rt_h - 1 - y, x)
        if zf is None or zg_v is None:
            continue
        tot += 1
        if int(zf + 0.5) == 0:
            zeros_fbo += 1
        if zg_v == 0:
            zeros_guest += 1
print(f"box total={tot} fbo_depth0={zeros_fbo} ({zeros_fbo * 100.0 / tot:.1f}%) "
      f"guest_depth0={zeros_guest} ({zeros_guest * 100.0 / tot:.1f}%)")
