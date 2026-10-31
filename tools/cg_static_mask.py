#!/usr/bin/env python3
"""t_3de68379: static-mask frame comparison between two stage runs.

Method (tools/tc3_static_mask.py, commit 7edd466, made parameterised here):
characters animate, so full-frame byte identity is not achievable.  Build
STATIC MASK(run) = pixels byte-identical between the run's OWN two captures
(t1 vs t2) - terrain / UI / static props - and compare the two runs ONLY on
the intersection of their masks.  Static scene content must match exactly;
animation-only differences are masked out.

usage: tools/cg_static_mask.py <labelA> <labelB>
       (expects evidence/t_3de68379/static/<label>/{a1.ppm,a2.ppm})
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EV = os.path.join(ROOT, "evidence", "t_3de68379", "static")


def readppm(p):
    f = open(p, "rb")
    assert f.readline().strip() == b"P6", p
    line = f.readline()
    while line.startswith(b"#"):
        line = f.readline()
    w, h = map(int, line.split())
    mx = int(f.readline())
    return w, h, f.read()


def load(label):
    a1 = os.path.join(EV, label, "a1.ppm")
    a2 = os.path.join(EV, label, "a2.ppm")
    w, h, d1 = readppm(a1)
    w2, h2, d2 = readppm(a2)
    assert (w, h) == (w2, h2), "size mismatch inside " + label
    return w, h, d1, d2


def main():
    la, lb = sys.argv[1:3]
    w, h, a1, a2 = load(la)
    w2, h2, b1, b2 = load(lb)
    assert (w, h) == (w2, h2), "size mismatch between runs"
    n = w * h
    mask = bytearray(n)
    static_a = static_b = 0
    for i in range(n):
        o = i * 3
        ma = a1[o:o + 3] == a2[o:o + 3]
        mb = b1[o:o + 3] == b2[o:o + 3]
        static_a += ma
        static_b += mb
        mask[i] = ma and mb
    cov = sum(mask)
    diffs = []
    band = [0] * ((h + 39) // 40)
    for i in range(n):
        if not mask[i]:
            continue
        o = i * 3
        if a1[o:o + 3] != b1[o:o + 3]:
            diffs.append(i)
            band[(i // w) // 40] += 1
    print("run %s: %dx%d, own-static pixels %d (%.1f%%)" % (la, w, h, static_a,
                                                            100.0 * static_a / n))
    print("run %s: %dx%d, own-static pixels %d (%.1f%%)" % (lb, w, h, static_b,
                                                            100.0 * static_b / n))
    print("intersection mask: %d px (%.1f%% of frame)" % (cov, 100.0 * cov / n))
    print("STATIC-MASK DIFF: %d px (%.4f%% of masked)" %
          (len(diffs), 100.0 * len(diffs) / max(cov, 1)))
    if band and any(band):
        print("by y-band (40 rows):", [(i * 40, c) for i, c in enumerate(band) if c])
    if diffs:
        rows = {}
        for i in diffs[:2000]:
            rows.setdefault(i // w, 0)
            rows[i // w] += 1
        print("hottest rows:", sorted(rows.items(), key=lambda kv: -kv[1])[:10])
    # where would a diff be WITHOUT the mask (for the operator's context)
    raw = sum(1 for i in range(n)
              if a1[i * 3:i * 3 + 3] != b1[i * 3:i * 3 + 3])
    print("raw (unmasked) frame difference: %d px (%.2f%%)" %
          (raw, 100.0 * raw / n))
    return 0 if not diffs else 1


if __name__ == "__main__":
    sys.exit(main())
