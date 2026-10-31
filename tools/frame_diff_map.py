#!/usr/bin/env python3
"""Frame-diff heatmap: localize which screen regions change between two frames.

Usage: frame_diff_map.py a.ppm b.ppm out.png [thr]

Prints a coarse 10x6 grid of changed-pixel counts per cell (row-major), and
writes a heatmap PNG (red = changed pixels).
"""
import sys

from PIL import Image

a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
out = sys.argv[3]
thr = int(sys.argv[4]) if len(sys.argv) > 4 else 6
if a.size != b.size:
    print("size mismatch", a.size, b.size)
    sys.exit(1)
W, H = a.size
pa = a.load()
pb = b.load()
heat = Image.new("RGB", (W, H))
ph = heat.load()
GX, GY = 10, 6
grid = [[0] * GX for _ in range(GY)]
total = 0
for y in range(H):
    for x in range(W):
        ra, ga, ba = pa[x, y]
        rb, gb, bb = pb[x, y]
        d = max(abs(ra - rb), abs(ga - gb), abs(ba - bb))
        if d > thr:
            total += 1
            ph[x, y] = (255, 0, 0)
            grid[y * GY // H][x * GX // W] += 1
        else:
            ph[x, y] = pb[x, y]
heat.save(out)
print("changed", total, "of", W * H)
for row in grid:
    print(" ".join(f"{c:6d}" for c in row))
