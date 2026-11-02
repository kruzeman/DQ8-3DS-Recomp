#!/usr/bin/env python3
"""t_d559a510: best-match diff of the last N frames of two capture arms."""
import glob
import os
import sys

from PIL import Image, ImageChops


def frames(d, n):
    fs = sorted(glob.glob(d + "/f_*.ppm"))[-n:]
    return [(os.path.basename(p), Image.open(p).convert("RGB")) for p in fs]


def diff(a, b):
    x = ImageChops.difference(a, b)
    h = x.histogram()
    npx = a.size[0] * a.size[1]
    nd = sum(sum(h[c * 256 + 1:(c + 1) * 256]) for c in range(3))
    mx = max([v for c in range(3) for v in range(256) if h[c * 256 + v]] or [0])
    return 100.0 * nd / (3.0 * npx), mx, nd


def main():
    pa, pb = sys.argv[1], sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 22
    A, B = frames(pa, n), frames(pb, n)
    print("=== %s vs %s  (last %d frames, best match per frame)" % (pa, pb, n))
    worst = 0.0
    wf = None
    for an, a in A:
        best = min((diff(a, b) + (bn,) for bn, b in B))
        if best[0] > worst:
            worst, wf = best[0], (an, best)
        print("  %s -> %s  %7.3f%% max=%d px=%d" % (an, best[3], best[0], best[1], best[2]))
    print("  worst %.3f%% at %s" % (worst, wf))


main()
