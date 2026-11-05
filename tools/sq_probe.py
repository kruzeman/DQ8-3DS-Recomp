#!/usr/bin/env python3
"""t_e216fc37 probe helper: convert a PPM (or crop) and print key pixels.

usage: python3 tools/sq_probe.py <ppm> [out.png] [x,y ...]
Prints the PPM size, then the RGB of the requested screen pixels (default the
splat probe and neighbours).
"""
import sys
from PIL import Image

src = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else None
pts = sys.argv[3:]
im = Image.open(src).convert("RGB")
print("size", im.size)
if out:
    im.save(out)
    print("wrote", out)
px = im.load()
W, H = im.size
if not pts:
    pts = ["270,180", "300,150", "270,200", "100,60", "200,230"]
for p in pts:
    x, y = (int(v) for v in p.split(","))
    if 0 <= x < W and 0 <= y < H:
        print("px %d,%d = %s" % (x, y, px[x, y]))
# dark share of the whole top screen (same metric the renderer cards quote)
n = W * H
dark = 0
for y in range(H):
    for x in range(W):
        r, g, b = px[x, y]
        if (r * 299 + g * 587 + b * 114) // 1000 < 24:
            dark += 1
print("dark%% = %.2f" % (100.0 * dark / n))
