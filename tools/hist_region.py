#!/usr/bin/env python3
"""Histogram of pixel max-channel values in a region of a PPM/PNG.

Usage: hist_region.py file.ppm [x0,y0,x1,y1 fractions]
Prints bucket counts: 0, 1-2, 3-5, 6-8, 9-16, 17-32, 33-64, 65-128, 129-255.
"""
import sys

from PIL import Image

img = Image.open(sys.argv[1]).convert("RGB")
W, H = img.size
if len(sys.argv) > 2:
    x0, y0, x1, y1 = (float(v) for v in sys.argv[2].split(","))
else:
    x0, y0, x1, y1 = 0.0, 0.5, 1.0, 1.0
box = (int(x0 * W), int(y0 * H), int(x1 * W), int(y1 * H))
reg = img.crop(box)
buckets = [0] * 9
edges = [1, 3, 6, 9, 17, 33, 65, 129, 256]
for (r, g, b) in reg.getdata():  # type: ignore[reportArgumentType]
    m = max(r, g, b)
    for i, e in enumerate(edges):
        if m < e:
            buckets[i] += 1
            break
print(sys.argv[1], "box", box, "px", reg.size[0] * reg.size[1])
labels = ["0", "1-2", "3-5", "6-8", "9-16", "17-32", "33-64", "65-128", "129-255"]
for l, c in zip(labels, buckets):
    print(f"  {l:>8}: {c}")
