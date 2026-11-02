#!/usr/bin/env python3
"""Crop + upscale a region of a PPM/PNG: crop.py in out x0 y0 x1 y1 scale"""
import sys
from PIL import Image
inp, out = sys.argv[1], sys.argv[2]
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
s = int(sys.argv[7]) if len(sys.argv) > 7 else 3
im = Image.open(inp).convert("RGB").crop((x0, y0, x1, y1))
im = im.resize((im.width * s, im.height * s), Image.NEAREST)
im.save(out)
print(out, im.size)
