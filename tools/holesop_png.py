#!/usr/bin/env python3
"""t_a22413f1: PPM -> PNG convert + per-frame stats (top colours)."""
import sys
from PIL import Image
import numpy as np

for arg in sys.argv[1:]:
    src, dst = arg.split(":")
    im = Image.open(src).convert("RGB")
    im.save(dst)
    a = np.asarray(im).astype(int)
    u = np.unique(a.reshape(-1, 3), axis=0)
    print("%s -> %s  size=%s mean=%.1f uniq_colours=%d"
          % (src, dst, im.size, a.mean(), len(u)))
