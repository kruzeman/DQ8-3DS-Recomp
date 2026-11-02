#!/usr/bin/env python3
"""t_a22413f1: pure-stdlib PPM (P6) differ — no numpy/PIL needed on this host.

usage: holesop_ppmdiff.py A.ppm B.ppm
prints: size, px differing by max-channel>30 and by sum-of-channels>30,
mean |d|, max |d|, and the first few differing pixels.
"""
import sys


def load(p):
    with open(p, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit("%s: not a P6 PPM" % p)
    # parse three integers after the magic, skipping whitespace/comments
    i, vals = 2, []
    while len(vals) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        vals.append(int(data[i:j]))
        i = j
    i += 1  # single whitespace after maxval
    w, h, _ = vals
    return w, h, data[i:i + w * h * 3]


def main():
    wa, ha, a = load(sys.argv[1])
    wb, hb, b = load(sys.argv[2])
    if (wa, ha) != (wb, hb):
        print("size mismatch %dx%d vs %dx%d" % (wa, ha, wb, hb))
        return 1
    n = wa * ha
    m30 = s30 = 0
    ssum = 0
    mx = 0
    first = []
    for k in range(n):
        o = 3 * k
        d0 = abs(a[o] - b[o])
        d1 = abs(a[o + 1] - b[o + 1])
        d2 = abs(a[o + 2] - b[o + 2])
        dm = max(d0, d1, d2)
        ds = d0 + d1 + d2
        ssum += ds
        if dm > mx:
            mx = dm
        if dm > 30:
            m30 += 1
        if ds > 30:
            s30 += 1
            if len(first) < 4:
                x, y = k % wa, k // wa
                first.append("(%d,%d) A=(%d,%d,%d) B=(%d,%d,%d)" % (
                    x, y, a[o], a[o + 1], a[o + 2], b[o], b[o + 1], b[o + 2]))
    print("%s vs %s  %dx%d" % (sys.argv[1], sys.argv[2], wa, ha))
    print("max-channel>30: %d px (%.3f%%)" % (m30, 100.0 * m30 / n))
    print("sum-channel>30: %d px (%.3f%%)" % (s30, 100.0 * s30 / n))
    print("mean |d| (sum/3px): %.4f   max |d|: %d" % (ssum / (3.0 * n), mx))
    for line in first:
        print("  differing: " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
