#!/usr/bin/env python3
"""Is the residual zero depth inside the FBO attachment-1, or only in guest
memory (i.e. introduced by the depth write-back)?"""
import os
import struct
import sys

XLUT = (0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15)
YLUT = (0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a)

gl = sys.argv[1]
sw = sys.argv[2]
x0, y0, x1, y1, rt_w, rt_h = (int(v) for v in
                              open(os.path.join(gl, "z_fbo_last.box")).read().split())
bw, bh = x1 - x0 + 1, y1 - y0 + 1
zbuf = open(os.path.join(gl, "z_fbo_last.raw"), "rb").read()
gmeta = open(os.path.join(gl, "guest_meta.txt")).read().split()
gbpp = int(gmeta[gmeta.index("depth_bpp") + 1])
gdepth = open(os.path.join(gl, "guest_depth.raw"), "rb").read()
swmeta = open(os.path.join(sw, "guest_meta.txt")).read().split()
swbpp = int(swmeta[swmeta.index("depth_bpp") + 1])
swdepth = open(os.path.join(sw, "guest_depth.raw"), "rb").read()


def z_fbo(x, y):
    i = ((y - y0) * bw + (x - x0)) * 4
    return struct.unpack_from("<f", zbuf, i)[0] if 0 <= i + 4 <= len(zbuf) else None


def z_guest(raw, bpp, row, col):
    off = ((row >> 3) * (rt_w >> 3) + (col >> 3)) * 64 + XLUT[col & 7] + YLUT[row & 7]
    i = off * bpp
    if i + bpp > len(raw):
        return None
    return struct.unpack_from("<I" if bpp > 2 else "<H", raw, i)[0]


print("guest col | FBO z (GL y=200) | GL guest z (row=199) | SW guest z (row=199)")
for col in range(184, 194):
    print(f"  {col:3d} | {z_fbo(col, rt_h - 1 - 199)!s:>22} | "
          f"{z_guest(gdepth, gbpp, 199, col)!s:>14} | {z_guest(swdepth, swbpp, 199, col)}")

print("\nper GL y at guest col 189 (FBO z): ")
for y in range(0, 400, 40):
    print(f"  y={y:3d} row={rt_h - 1 - y:3d} fbo={z_fbo(189, y)} "
          f"gl_guest={z_guest(gdepth, gbpp, rt_h - 1 - y, 189)} "
          f"sw_guest={z_guest(swdepth, swbpp, rt_h - 1 - y, 189)}")
