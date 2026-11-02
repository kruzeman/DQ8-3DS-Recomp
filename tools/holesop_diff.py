#!/usr/bin/env python3
"""t_a22413f1: diff two PPM/PNG frames and report structure.

usage: holesop_diff.py A B [out.png]
"""
import sys
from PIL import Image
import numpy as np


def load(p):
    return np.asarray(Image.open(p).convert("RGB")).astype(np.int16)


a, b = load(sys.argv[1]), load(sys.argv[2])
if a.shape != b.shape:
    print("size mismatch %s vs %s" % (a.shape, b.shape))
    sys.exit(1)
d = np.abs(a - b)
m30 = d.max(axis=2) > 30
m0 = d.max(axis=2) > 0
print("frames %dx%d" % (a.shape[1], a.shape[0]))
print("  mean|d| = %.4f   max|d| = %d" % (d.mean(), d.max()))
print("  px >0 = %d (%.3f%%)   px >30 = %d (%.3f%%)"
      % (m0.sum(), 100.0 * m0.sum() / m0.size, m30.sum(), 100.0 * m30.sum() / m30.size))
ys, xs = np.nonzero(m30)
if len(ys):
    print("  >30 bbox x[%d,%d] y[%d,%d]" % (xs.min(), xs.max(), ys.min(), ys.max()))
    # row histogram in 10 bands, so the location of the disagreement is visible
    h = a.shape[0]
    band = h // 10 + 1
    hist = [int(m30[i:i + band].sum()) for i in range(0, h, band)]
    print("  >30 rows (10 bands): %s" % hist)
if len(sys.argv) > 3:
    h, w, _ = a.shape
    canvas = np.zeros((h, w * 3 + 8, 3), np.uint8)
    canvas[:, :w] = np.clip(a, 0, 255).astype(np.uint8)
    canvas[:, w + 4:2 * w + 4] = np.clip(b, 0, 255).astype(np.uint8)
    canvas[:, 2 * w + 8:] = np.repeat(np.where(m30, 255, 0).astype(np.uint8)[:, :, None], 3, axis=2)
    Image.fromarray(canvas).resize((canvas.shape[1] * 2, canvas.shape[0] * 2), Image.NEAREST).save(sys.argv[3])
    print("  wrote %s (A | B | mask>30)" % sys.argv[3])
