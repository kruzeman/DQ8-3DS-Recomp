#!/usr/bin/env python3
"""Interpret the DQ8_GL_DUMP_DIR flush snapshots (GL coordinates) against the
software reference screen PPM.

Conventions (verified in src/pica_vs.c / src/pica_gl.c):
  guest row r (top-down)  <-> GL y = rt_h-1-r   (raster y is bottom-up)
  guest col c             <-> GL x = c
  screen(x_s, y_s)        <-> guest(row = x_s, col = 2*(239 - y_s))

Usage:
  analyze_dump.py <dumpdir> <sw_screen.ppm> [<gl_screen.ppm>]
"""
import os
import sys
from PIL import Image

dumpdir = sys.argv[1]
sw = Image.open(sys.argv[2]).convert("RGB")
gl = Image.open(sys.argv[3]).convert("RGB") if len(sys.argv) > 3 else None


def read(name):
    p = os.path.join(dumpdir, name)
    if not os.path.exists(p):
        print("MISSING", p)
        return None
    with open(p, "rb") as f:
        return f.read()


def box_of(name):
    b = read(name)
    if b is None:
        return None
    x0, y0, x1, y1, rw, rh = (int(v) for v in b.split())
    return x0, y0, x1, y1, rw, rh


cb = box_of("color_fbo_last.box")
print("color box:", cb)
cbuf = read("color_fbo_last.raw")
if cb is None or cbuf is None:
    sys.exit(1)
x0, y0, x1, y1, rt_w, rt_h = cb
bw = x1 - x0 + 1
bh = y1 - y0 + 1
assert len(cbuf) >= bw * bh * 4, (len(cbuf), bw * bh * 4)


def fbo_px(x, y):
    """FBO pixel (GL coords) -> (r, g, b, a) or None when outside the box."""
    if not (x0 <= x <= x1 and y0 <= y <= y1):
        return None
    i = ((y - y0) * bw + (x - x0)) * 4
    return cbuf[i], cbuf[i + 1], cbuf[i + 2], cbuf[i + 3]


def sw_px(xs, ys):
    return sw.getpixel((xs, ys))


def gl_px(xs, ys):
    return gl.getpixel((xs, ys)) if gl else None


# guest color buffer (post write-back of the last flush)
gbuf = read("color_guest_last.raw")
print("guest color bytes:", None if gbuf is None else len(gbuf))


def guest_px(row, col):
    """guest render target pixel: 8x8 Morton blocks, block-row-major,
    within-block Morton (xlut/ylut), RGBA8 stored A,B,G,R (pica_put_pixel)."""
    if gbuf is None:
        return None
    xlut = (0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15)
    ylut = (0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a)
    blocks_x = rt_w // 8
    off = ((row >> 3) * blocks_x + (col >> 3)) * 64 + xlut[col & 7] + ylut[row & 7]
    i = off * 4
    if i + 4 > len(gbuf):
        return None
    a, b, g, r = gbuf[i], gbuf[i + 1], gbuf[i + 2], gbuf[i + 3]
    return r, g, b, a


# depth snapshots
zb = box_of("z_fbo_last.box")
zbuf = read("z_fbo_last.raw")
zg = read("z_guest_last.raw")
print("z box:", zb, "z fbo bytes:", None if zbuf is None else len(zbuf),
      "z guest bytes:", None if zg is None else len(zg))


def z_at(x, y):
    if zb is None or zbuf is None:
        return None
    zx0, zy0, zx1, zy1, _, _ = zb
    if not (zx0 <= x <= zx1 and zy0 <= y <= zy1):
        return None
    i = ((y - zy0) * (zx1 - zx0 + 1) + (x - zx0)) * 4
    import struct
    return struct.unpack_from("<f", zbuf, i)[0]


def zguest(row, col):
    """guest depth buffer pixel (same Morton order; 4 bytes per entry)."""
    if zg is None:
        return None
    xlut = (0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15)
    ylut = (0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a)
    blocks_x = rt_w // 8
    off = ((row >> 3) * blocks_x + (col >> 3)) * 64 + xlut[col & 7] + ylut[row & 7]
    i = off * 4
    if i + 4 > len(zg):
        return None
    import struct
    return struct.unpack_from("<I", zg, i)[0]


def near(a, b, tol=8):
    if a is None or b is None:
        return False
    return all(abs(a[i] - b[i]) <= tol for i in range(3))


bands = [(357, 400, "GL y357..399 (the missing strip)"),
         (300, 357, "GL y300..356 (just below strip)"),
         (200, 300, "GL y200..299 (dialog middle)"),
         (100, 200, "GL y100..199"),
         (0, 100, "GL y0..99 (dialog bottom)")]

print("\n== FBO color vs SW screen (screen x_s = 399-y, y_s = 239 - x//2) ==")
for ya, yb, label in bands:
    tot = match_sw = match_gl = n = 0
    for y in range(ya, yb):
        for x in range(x0, min(x1, 189) + 1, 3):
            xs = rt_h - 1 - y
            ys = 239 - x // 2
            if not (0 <= xs < 400 and 0 <= ys < 240):
                continue
            f = fbo_px(x, y)
            if f is None:
                continue
            n += 1
            s = sw_px(xs, ys)
            if near(f, s):
                match_sw += 1
            if gl is not None and near(f, gl_px(xs, ys)):
                match_gl += 1
    if n:
        print(f"  {label:38s} n={n:5d} match_sw={match_sw * 100.0 / n:5.1f}%"
              + (f" match_gl_frame={match_gl * 100.0 / n:5.1f}%" if gl else ""))

print("\n== FBO vs guest color at the same pixels (last flush, post write-back) ==")
for ya, yb, label in bands:
    n = same = 0
    for y in range(ya, yb):
        for x in range(x0, min(x1, 189) + 1, 3):
            f = fbo_px(x, y)
            g = guest_px(rt_h - 1 - y, x)
            if f is None or g is None:
                continue
            n += 1
            if f == g:
                same += 1
    if n:
        print(f"  {label:38s} n={n:5d} identical={same * 100.0 / n:5.1f}%")

print("\n== depth: FBO attachment-1 vs guest depth (GL coords) ==")
for ya, yb, label in bands:
    n = same = 0
    samples = []
    for y in range(ya, yb, 4):
        for x in range(x0, min(x1, 189) + 1, 8):
            z = z_at(x, y)
            zg_v = zguest(rt_h - 1 - y, x)
            if z is None or zg_v is None:
                continue
            n += 1
            if int(z + 0.5) == zg_v:
                same += 1
            if len(samples) < 4:
                samples.append((x, y, round(z, 1), zg_v))
    if n:
        print(f"  {label:38s} n={n:4d} z_fbo==z_guest={same * 100.0 / n:5.1f}% "
              f"samples(x,y,zfbo,zguest)={samples}")

print("\n== pixels SW draws (non-black) but GL FBO keeps black ==")
for ya, yb, label in bands:
    sw_nb = black = match = other = 0
    zb_stat = {}
    ex = []
    for y in range(ya, yb):
        for x in range(x0, min(x1, 189) + 1):
            xs = rt_h - 1 - y
            ys = 239 - x // 2
            if not (0 <= xs < 400 and 0 <= ys < 240):
                continue
            s = sw_px(xs, ys)
            if sum(s) <= 30:
                continue
            sw_nb += 1
            f = fbo_px(x, y)
            if f is None:
                continue
            z = z_at(x, y)
            if sum(f[:3]) <= 10:
                black += 1
                key = "z=%d" % (int(z) if z is not None else -1)
                zb_stat[key] = zb_stat.get(key, 0) + 1
                if len(ex) < 5:
                    ex.append((x, y, "screen", xs, ys, "sw", s, "fbo", f,
                               "zfbo", None if z is None else int(z),
                               "zguest", zguest(rt_h - 1 - y, x)))
            elif near(f, s):
                match += 1
            else:
                other += 1
    if sw_nb:
        print(f"  {label:38s} sw_nonblack={sw_nb:5d} fbo_black={black:5d} "
              f"match={match:5d} other={other:5d}")
        if zb_stat:
            top = sorted(zb_stat.items(), key=lambda kv: -kv[1])[:6]
            print(f"      depth at the black ones: {top}")
        for e in ex[:3]:
            print("      ex:", e)
