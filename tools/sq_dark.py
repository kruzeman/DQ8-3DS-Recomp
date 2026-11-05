#!/usr/bin/env python3
"""t_e216fc37: report the dark regions of a 400x240 top-screen capture as a
coarse grid, so a probe pixel inside the remaining black area can be chosen.

usage: python3 tools/sq_dark.py <ppm|png> [thresh]
"""
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert("RGB")
t = int(sys.argv[2]) if len(sys.argv) > 2 else 24
W, H = im.size
px = im.load()
print("size", im.size, "thresh", t)
for y in range(0, H, 4):
    row = []
    for x in range(0, W, 4):
        r, g, b = px[x, y]
        row.append("#" if (r * 299 + g * 587 + b * 114) // 1000 < t else ".")
    print("%3d %s" % (y, "".join(row)))
