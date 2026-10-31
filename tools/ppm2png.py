#!/usr/bin/env python3
"""Convert one or more PPM frames to PNG (scratch) for visual inspection.

usage: ppm2png.py <out_dir> <ppm> [<ppm> ...]
"""
import os
import sys
from PIL import Image

out_dir = sys.argv[1]
os.makedirs(out_dir, exist_ok=True)
for p in sys.argv[2:]:
    name = os.path.basename(p)
    if name.endswith(".bottom.ppm"):
        stem = name[: -len(".bottom.ppm")]
    else:
        stem = name[: -len(".ppm")]
    dst = os.path.join(out_dir, stem + ".png")
    im = Image.open(p)
    im.save(dst)
    print(dst, im.size)
