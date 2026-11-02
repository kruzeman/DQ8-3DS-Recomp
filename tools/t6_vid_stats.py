#!/usr/bin/env python3
"""Per-frame panel statistics for the operator video (no numpy).

usage: vid_stats.py <dir> [x0 y0 x1 y1]
"""
import sys, glob
from PIL import Image

d = sys.argv[1]
box = tuple(int(v) for v in sys.argv[2:6]) if len(sys.argv) > 5 else None
files = sorted(glob.glob(d + "/fr_*.png"))
prev = None
print("frame  mean   p90   max  changed_vs_prev")
for i, f in enumerate(files):
    im = Image.open(f).convert("L")
    if box:
        im = im.crop(box)
    px = list(im.getdata())
    n = len(px)
    s = sorted(px)
    mean = sum(px) / n
    p90 = s[int(n * 0.9)]
    if prev is not None:
        ch = sum(1 for a, b in zip(px, prev) if abs(a - b) > 8)
    else:
        ch = -1
    print("%3d  %6.2f %6.1f %5d  %6d" % (i + 1, mean, p90, s[-1], ch))
    prev = px
