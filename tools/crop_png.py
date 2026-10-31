#!/usr/bin/env python3
"""Crop a region of a PPM and save as PNG for visual inspection.
Usage: crop_png.py in.ppm out.png x0 y0 x1 y1 [scale]"""
import sys
from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
scale = int(sys.argv[7]) if len(sys.argv) > 7 else 1
im = Image.open(src).convert("RGB").crop((x0, y0, x1, y1))
if scale != 1:
    im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
im.save(dst)
print("wrote", dst, im.size)
