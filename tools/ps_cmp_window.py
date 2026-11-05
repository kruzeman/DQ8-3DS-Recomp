#!/usr/bin/env python3
"""t_aaecbb52 present-spike frame comparison (pure stdlib).

Compares two window images produced by DQ8_PRESENT_DUMP_PPM for the SAME
deterministic frame: one with DQ8_PRESENT_SPIKE=1, one with it off.

Reported:
  * mean |d| over the top-screen rect (0,0)-(799,479);
  * the integer shift (dx,dy) in [-4,4] that minimises mean |d| — 0,0 means
    the framing/registration is the guest path's;
  * the fraction of 2x2 blocks in the rect that are uniform (a nearest-
    neighbour 2x upscale of a 400x240 image makes ~100% of them uniform, a
    real supersampled 800x480 image far fewer) — the "no 2x2 blocks" test.
"""
import sys

W, H = 800, 960
TOP_W, TOP_H = 800, 480


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit("%s: not a P6 PPM" % path)
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    while data[i:i + 1].isspace():
        i += 1
    w, h, mx = fields
    if mx != 255:
        raise SystemExit("maxval %d unsupported" % mx)
    return w, h, data[i:i + w * h * 3]


def top_rect(w, h, pix):
    px = []
    for y in range(min(h, TOP_H)):
        row = pix[y * w * 3:(y + 1) * w * 3]
        px.append(row[:min(w, TOP_W) * 3])
    return px


def mean_abs(a, b, dx, dy):
    n = 0
    tot = 0
    for y in range(4, TOP_H - 4):
        ra = a[y]
        rb = b[y + dy]
        lo = 4 + max(dx, 0)
        hi = TOP_W - 4 + min(dx, 0)
        for x in range(lo, hi):
            o = x * 3
            o2 = (x + dx) * 3
            tot += abs(ra[o] - rb[o2]) + abs(ra[o + 1] - rb[o2 + 1]) + \
                abs(ra[o + 2] - rb[o2 + 2])
            n += 3
    return tot / float(n) if n else 0.0


def uniform_2x2(rows):
    uni = 0
    tot = 0
    for y in range(0, TOP_H - 1, 2):
        r0, r1 = rows[y], rows[y + 1]
        for x in range(0, TOP_W - 1, 2):
            o = x * 3
            o2 = o + 3
            tot += 1
            if r0[o] == r0[o2] == r1[o] == r1[o2] and \
               r0[o + 1] == r0[o2 + 1] == r1[o + 1] == r1[o2 + 1] and \
               r0[o + 2] == r0[o2 + 2] == r1[o + 2] == r1[o2 + 2]:
                uni += 1
    return uni / float(tot), tot


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: spike_compare.py ON.ppm OFF.ppm")
    on_p, off_p = sys.argv[1], sys.argv[2]
    w1, h1, p1 = read_ppm(on_p)
    w2, h2, p2 = read_ppm(off_p)
    if (w1, h1) != (w2, h2) or (w1, h1) != (W, H):
        raise SystemExit("unexpected sizes %dx%d / %dx%d" % (w1, h1, w2, h2))
    on = top_rect(w1, h1, p1)
    off = top_rect(w2, h2, p2)
    best = None
    for dy in range(-4, 5):
        for dx in range(-4, 5):
            d = mean_abs(on, off, dx, dy)
            if best is None or d < best[0]:
                best = (d, dx, dy)
    d00 = mean_abs(on, off, 0, 0)
    u_on, tot = uniform_2x2(on)
    u_off, _ = uniform_2x2(off)
    # more than 8 levels of difference = "content differs", not just AA
    big = 0
    cnt = 0
    for y in range(TOP_H):
        for x in range(TOP_W):
            o = x * 3
            dd = abs(on[y][o] - off[y][o]) + abs(on[y][o + 1] - off[y][o + 1]) + \
                abs(on[y][o + 2] - off[y][o + 2])
            cnt += 1
            if dd > 24:
                big += 1
    print("frame pair        : %s  vs  %s" % (on_p, off_p))
    print("top rect          : %dx%d" % (TOP_W, TOP_H))
    print("mean |d| at 0,0   : %.3f" % d00)
    print("best shift        : dx=%d dy=%d mean|d|=%.3f" % (best[1], best[2], best[0]))
    print("pixels >24 levels : %.2f%%" % (100.0 * big / cnt))
    print("uniform 2x2 blocks: spike=%.2f%%  guest=%.2f%% (%d blocks)" %
          (100.0 * u_on, 100.0 * u_off, tot))


if __name__ == "__main__":
    main()
