#!/usr/bin/env python3
"""Convert a probe PPM to PNG for visual inspection (t_bc6b4eb7)."""
import sys
from PIL import Image

for src in sys.argv[1:]:
    dst = src.rsplit(".", 1)[0] + ".png"
    Image.open(src).save(dst)
    print(dst)
