#!/usr/bin/env python3
"""Compare GL's own post-write-back depth snapshot (z_guest_last.raw) with the
runtime's capture-time dump (guest_depth.raw): who differs, where?"""
import os
import struct
import sys
from collections import Counter

XLUT = (0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15)
YLUT = (0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a)

d = sys.argv[1]
x0, y0, x1, y1, rt_w, rt_h = (int(v) for v in
                              open(os.path.join(d, "z_fbo_last.box")).read().split())
bw, bh = x1 - x0 + 1, y1 - y0 + 1
zbuf = open(os.path.join(d, "z_fbo_last.raw"), "rb").read()   # attachment-1 readback
gl_guest = open(os.path.join(d, "z_guest_last.raw"), "rb").read()  # guest after GL depth flush
cap = open(os.path.join(d, "guest_depth.raw"), "rb").read()    # guest at capture
meta = open(os.path.join(d, "guest_meta.txt")).read().split()
bpp = int(meta[meta.index("depth_bpp") + 1])


def off(row, col):
    return (((row >> 3) * (rt_w >> 3) + (col >> 3)) * 64 +
            XLUT[col & 7] + YLUT[row & 7]) * bpp


def rd(raw, row, col):
    i = off(row, col)
    if i + bpp > len(raw):
        return None
    return struct.unpack_from("<I" if bpp > 2 else "<H", raw, i)[0] & 0xFFFFFF


def fbo(x, y):
    i = ((y - y0) * bw + (x - x0)) * 4
    if i < 0 or i + 4 > len(zbuf):
        return None
    return int(struct.unpack_from("<f", zbuf, i)[0] + 0.5)


diff_gl_vs_cap = Counter()
diff_fbo_vs_gl = Counter()
diff_fbo_vs_cap = Counter()
for row in range(rt_h):
    for col in range(rt_w):
        a = rd(gl_guest, row, col)
        b = rd(cap, row, col)
        f = fbo(col, rt_h - 1 - row)
        if a is not None and b is not None and a != b:
            diff_gl_vs_cap[col] += 1
        if f is not None and a is not None and f != a:
            diff_fbo_vs_gl[col] += 1
        if f is not None and b is not None and f != b:
            diff_fbo_vs_cap[col] += 1
print("GL-flush guest vs capture-time guest: total=",
      sum(diff_gl_vs_cap.values()), "cols=", sorted(diff_gl_vs_cap.items())[:20])
print("attachment1 vs GL-flush guest:        total=",
      sum(diff_fbo_vs_gl.values()), "cols=", sorted(diff_fbo_vs_gl.items())[:20])
print("attachment1 vs capture-time guest:    total=",
      sum(diff_fbo_vs_cap.values()), "cols=", sorted(diff_fbo_vs_cap.items())[:20])
