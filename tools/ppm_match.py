#!/usr/bin/env python3
"""t_d559a510: best-match frame comparison between two capture arms.

usage: ppm_match.py <dirA> <dirB> [first] [last]

For every frame f_*_*.ppm in dirA (sorted, optional index window), finds the
frame in dirB with the smallest number of differing pixels and prints it. This
separates "same screen at a different animation phase" (some pairing is
pixel-identical or near) from "structurally different rendering" (no pairing
comes close).
"""
import sys
import glob
import os
import re

from PIL import Image, ImageChops


def frames(d):
    fs = sorted(glob.glob(os.path.join(d, 'f_*.ppm')))
    out = []
    for f in fs:
        m = re.search(r'f_(\d+)_', f)
        if os.path.getsize(f) != 288015:   # truncated mid-write capture
            continue
        out.append((int(m.group(1)) if m else 0, f))
    return out


def diff_stats(ia, ib):
    d = ImageChops.difference(ia, ib)
    # count differing pixels: any channel != 0
    r, g, b = d.split()
    m = ImageChops.lighter(ImageChops.lighter(r, g), b)
    hist = m.histogram()
    cnt = sum(hist[1:])
    maxd = 0
    for v in range(255, 0, -1):
        if hist[v]:
            maxd = v
            break
    return cnt, maxd


def main():
    a, b = sys.argv[1], sys.argv[2]
    fa, fb = frames(a), frames(b)
    lo = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    hi = int(sys.argv[4]) if len(sys.argv) > 4 else len(fa)
    cache = [(n, Image.open(p).convert('RGB')) for n, p in fb]
    worst = 0
    for n, p in fa[lo:hi]:
        ia = Image.open(p).convert('RGB')
        best = None
        for m, ib in cache:
            cnt, maxd = diff_stats(ia, ib)
            if best is None or cnt < best[0]:
                best = (cnt, maxd, m)
                if cnt == 0:
                    break
        print('A_%03d -> B_%03d diff_px=%d maxd=%d (%.3f%%)'
              % (n, best[2], best[0], best[1], 100.0 * best[0] / (ia.width * ia.height)))
        worst = max(worst, best[0])
    print('worst_best_match_diff_px=%d' % worst)


main()
