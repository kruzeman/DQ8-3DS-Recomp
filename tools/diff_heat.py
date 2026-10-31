#!/usr/bin/env python3
"""Render a diff heatmap between two PPMs: red = differs, green = same-but-nonblack."""
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
out = sys.argv[3]
w, h = a.size
pa, pb = a.load(), b.load()
img = Image.new("RGB", (w, h))
p = img.load()
for y in range(h):
    for x in range(w):
        ca, cb = pa[x, y], pb[x, y]
        if ca != cb:
            p[x, y] = (255, 0, 0)
        elif sum(ca) > 30:
            p[x, y] = (0, 90, 0)
        else:
            p[x, y] = (0, 0, 0)
img.save(out)
print("wrote", out)
