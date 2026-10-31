#!/usr/bin/env python3
"""Analyze a scanline row range of two PPMs to localize clipping.
Usage: analyze_clip.py a.ppm b.ppm [row]
For the given row (default 120), print runs of pixels for cols 0..60."""
import sys
from PIL import Image

a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
w, h = a.size
pa, pb = a.load(), b.load()
row = int(sys.argv[3]) if len(sys.argv) > 3 else 120

# Find box horizontal extent in image A (non-black columns anywhere in image)
def nonblack_cols(p):
    cols = []
    for x in range(w):
        for y in range(h):
            r, g, bl = p[x, y]
            if r + g + bl > 30:
                cols.append(x)
                break
    return cols
ca = nonblack_cols(pa)
cb = nonblack_cols(pb)
print(f"A nonblack cols: {ca[0] if ca else None}..{ca[-1] if ca else None} (n={len(ca)})")
print(f"B nonblack cols: {cb[0] if cb else None}..{cb[-1] if cb else None} (n={len(cb)})")

print(f"row {row}:  col | A(rgb) | B(rgb)")
for x in range(0, 70):
    mark = "  <<" if pa[x, row] != pb[x, row] else ""
    print(f"  {x:3d} | {pa[x,row]} | {pb[x,row]}{mark}")
