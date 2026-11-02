#!/usr/bin/env python3
"""Row/column mean-luminance profile to locate the panel border, then per-frame
panel-interior mean luminance across the operator video frames.

usage: t6_panel_track.py <dir> [--profile]
"""
import sys, glob
from PIL import Image

d = sys.argv[1]
prof = "--profile" in sys.argv
files = sorted(glob.glob(d + "/fr_*.png"))
im0 = Image.open(files[0]).convert("L")
w, h = im0.size
print("frame size", w, h)

if prof:
    px = list(im0.getdata())
    rows = [sum(px[y * w:(y + 1) * w]) / w for y in range(h)]
    cols = [sum(px[y * w + x] for y in range(h)) / h for x in range(w)]
    print("row profile:")
    for y in range(h):
        print("%3d %6.1f %s" % (y, rows[y], "#" * int(rows[y] / 6)))
    print("col peaks (top 20):")
    order = sorted(range(w), key=lambda x: -cols[x])[:20]
    for x in sorted(order):
        print("  col %3d  %6.1f" % (x, cols[x]))
