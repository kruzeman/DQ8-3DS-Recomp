#!/usr/bin/env python3
"""t_a22413f1 run3: phase-tolerant frame matcher (pure stdlib).

Compares two PPM frame directories by 8x8 box means (so guest/animation phase
shifts and 1-LSB noise do not dominate) and prints the best partner for each
frame plus the resulting full-resolution diff of the best pair.

usage: holesop_match2.py DIR_A DIR_B
"""
import os
import sys

STEP = 8


def load(p):
    with open(p, "rb") as f:
        data = f.read()
    i, vals = 2, []
    while len(vals) < 3:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        vals.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _ = vals
    return w, h, data[i:i + w * h * 3]


def blocks(p):
    w, h, pix = load(p)
    bw, bh = w // STEP, h // STEP
    out = []
    for by in range(bh):
        for bx in range(bw):
            s0 = s1 = s2 = 0
            for y in range(by * STEP, by * STEP + STEP):
                row = (y * w + bx * STEP) * 3
                for x in range(STEP):
                    o = row + x * 3
                    s0 += pix[o]
                    s1 += pix[o + 1]
                    s2 += pix[o + 2]
            n = STEP * STEP * 255.0
            out.append((s0 / n, s1 / n, s2 / n))
    return (w, h), out


def dist(a, b):
    return sum(abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2])
               for p, q in zip(a, b)) / len(a)


def main():
    da, db = sys.argv[1], sys.argv[2]
    fa = sorted(f for f in os.listdir(da) if f.endswith(".ppm"))
    fb = sorted(f for f in os.listdir(db) if f.endswith(".ppm"))
    A = [(f,) + blocks(os.path.join(da, f)) for f in fa]
    B = [(f,) + blocks(os.path.join(db, f)) for f in fb]
    print("%d frames vs %d frames, %dx%d block means step=%d"
          % (len(A), len(B), A[0][1][0] // STEP if A else 0,
             A[0][1][1] // STEP if A else 0, STEP))
    best = None
    for name, _, ba in A:
        d, partner = min((dist(ba, bb), bname) for bname, _, bb in B)
        if best is None or d < best[0]:
            best = (d, name, partner)
        print("A %-12s best B %-12s mean block |d| = %.4f" % (name, partner, d))
    print("BEST: A %s  <->  B %s   mean block |d| = %.4f" % (best[1], best[2], best[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
