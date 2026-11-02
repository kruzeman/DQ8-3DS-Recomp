#!/usr/bin/env python3
"""t_a22413f1: cross-run frame agreement.

For every frame in each run directory, compare it against the reference run's
frames: report how many are BYTE-IDENTICAL to some reference frame (the strong
statement — two backends at the same guest moment) and, for the rest, the best
pixel match available (phase-shifted runs never align exactly, so this
separates "different guest moment" from "different renderer output").

usage: holesop_cross.py <refdir> <dir> [<dir> ...]
"""
import sys, os, glob
import numpy as np
from PIL import Image


def load_dir(d):
    out = []
    for f in sorted(x for x in glob.glob(os.path.join(d, "t0*.ppm")) if ".bot." not in x):
        if os.path.getsize(f) < 288015:
            continue
        im = Image.open(f)
        out.append((os.path.basename(f), im.tobytes(), np.asarray(im.convert("RGB")).astype(np.int16)))
    return out


ref = load_dir(sys.argv[1])
ref_blob = {b for _, b, _ in ref}
ref_px = [a for _, _, a in ref]
print("reference %s: %d frames (%d distinct)" % (sys.argv[1], len(ref), len(ref_blob)))
for d in sys.argv[2:]:
    run = load_dir(d)
    exact = sum(1 for _, b, _ in run if b in ref_blob)
    best = []
    for _, _, a in run:
        best.append(min(float(np.abs(a - r).mean()) for r in ref_px))
    best = np.array(best)
    print("%s: %d frames, %d byte-identical to a reference frame (%.0f%%);"
          " best-phase residual mean|d| = %.3f median=%.3f worst=%.3f"
          % (d, len(run), exact, 100.0 * exact / max(1, len(run)),
             best.mean(), float(np.median(best)), best.max()))
