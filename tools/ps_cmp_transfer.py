#!/usr/bin/env python3
"""t_fdd26eba: does a scaled display-transfer destination decode to the same
picture as the guest's native destination bytes?

    python3 tools/ps_cmp_transfer.py <scaled.ppm> <native.ppm> [f]

The scaled destination is the transfer done at the internal scale (Azahar
rasterizer_cache.h:289-352: destination inherits res_scale), the native one is
the guest's own RGB8 destination written to guest RAM.  At scale f the scaled
destination is f x larger on both axes and every native texel is the box
average of the f x f block of scaled texels that covers it, so

    box_down(scaled, f) == native      (up to filtering rounding)

is the geometric check.  Also reports the same statistic for the two axis
mirrors, so a flipped/wrong-origin read is visible instead of hiding inside a
"close enough" number.
"""
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        d = f.read()
    if not d.startswith(b"P6"):
        raise SystemExit("%s: not a P6 PPM" % path)
    fields, i = [], 2
    while len(fields) < 3:
        while d[i:i + 1].isspace():
            i += 1
        if d[i:i + 1] == b"#":
            while d[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        fields.append(int(d[i:j]))
        i = j
    while d[i:i + 1].isspace():
        i += 1
    w, h, mx = fields
    return w, h, d[i:i + w * h * 3]


def box_down(w, h, px, f):
    nw, nh = w // f, h // f
    out = bytearray(nw * nh * 3)
    n = f * f
    for y in range(nh):
        for x in range(nw):
            acc = [0, 0, 0]
            for yy in range(y * f, (y + 1) * f):
                row = yy * w * 3
                for xx in range(x * f, (x + 1) * f):
                    o = row + xx * 3
                    acc[0] += px[o]
                    acc[1] += px[o + 1]
                    acc[2] += px[o + 2]
            o = (y * nw + x) * 3
            for c in range(3):
                out[o + c] = (acc[c] + n // 2) // n
    return nw, nh, bytes(out)


def mean_abs(w, h, a, b, fx=False, fy=False):
    tot = n = 0
    for y in range(h):
        sy = (h - 1 - y) if fy else y
        ra = a[sy * w * 3:(sy + 1) * w * 3]
        rb = b[y * w * 3:(y + 1) * w * 3]
        for x in range(w):
            sx = (w - 1 - x) if fx else x
            o = x * 3
            o2 = sx * 3
            tot += abs(ra[o] - rb[o2]) + abs(ra[o + 1] - rb[o2 + 1]) + \
                abs(ra[o + 2] - rb[o2 + 2])
            n += 3
    return tot / float(n)


def main():
    sc_path, nat_path = sys.argv[1], sys.argv[2]
    f = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    sw, sh, sp = read_ppm(sc_path)
    nw, nh, np_ = read_ppm(nat_path)
    print("scaled %dx%d  native %dx%d  f=%d" % (sw, sh, nw, nh, f))
    if (sw, sh) != (nw * f, nh * f):
        raise SystemExit("dimension mismatch: scaled is not native * f")
    w, h, down = box_down(sw, sh, sp, f)
    for name, fx, fy in (("identity", False, False), ("mirror-x", True, False),
                         ("mirror-y", False, True), ("rot-180", True, True)):
        print("  %-9s box_down(scaled) vs native: mean|d| = %.3f   "
              "max|d| = %d" % (name, mean_abs(w, h, down, np_, fx, fy),
                               max(max(abs(down[i] - np_[i]) for i in range(k, k + 3))
                                   for k in range(0, w * h * 3, 3))))


if __name__ == "__main__":
    main()
