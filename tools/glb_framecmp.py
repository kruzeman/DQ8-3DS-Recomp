#!/usr/bin/env python3
"""t_d559a510: frame-level A/B of two capture arms (GL fixed-function blend vs
the shader/SW blend reference) from the same savestate.

The wall clock is NOT a valid phase reference here (the menu sits on an animated
3D world map, so two runs are at different animation phases; an A/A control
differs on ~70 % of pixels).  Two robust measures are used instead:

  * index-aligned: f_00N of arm A vs f_00N of arm B.  Frame 1 is the first
    display transfer after --load-state in both arms, so it is deterministic.
  * best-match: for every frame of arm A, the arm-B frame with the smallest
    difference (animation phase removed).  A near-zero best match for every
    frame means the two arms render the same image, just at a different phase.

usage: glb_framecmp.py <dirA> <dirB> [max_pairs]
"""
import glob
import os
import sys

from PIL import Image, ImageChops


def load(p):
    return Image.open(p).convert("RGB")


def diff(a, b):
    if a.size != b.size:
        return 1.0, 255, 1
    d = ImageChops.difference(a, b)
    hist = d.histogram()
    npx = a.size[0] * a.size[1]
    ndiff = 0
    mx = 0
    for ch in range(3):
        h = hist[ch * 256:(ch + 1) * 256]
        ndiff += sum(h[1:])
        for v in range(255, 0, -1):
            if h[v]:
                mx = max(mx, v)
                break
    return 100.0 * ndiff / (3.0 * npx), mx, ndiff


def frames(d):
    out = []
    for p in sorted(glob.glob(os.path.join(d, "f_*.ppm"))):
        out.append((os.path.basename(p).split("_")[1], p))
    return out


def main():
    A = frames(sys.argv[1])
    B = frames(sys.argv[2])
    lim = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    print("armA=%d frames  armB=%d frames" % (len(A), len(B)))
    print("\nindex-aligned (first %d):" % lim)
    print("  n   %-12s %-8s %-6s %s" % ("size", "%diff", "max", "px"))
    for i in range(min(lim, len(A), len(B))):
        a = load(A[i][1])
        b = load(B[i][1])
        pct, mx, nd = diff(a, b)
        print("  %-3d %-12s %8.3f%% %6d %d" % (i + 1, "%dx%d" % a.size, pct, mx, nd))
    print("\nbest match of each armA frame inside armB (phase-free):")
    Bi = [(n, load(p)) for n, p in B[:min(len(B), 40)]]
    worst = 0.0
    for n, p in A[:min(len(A), 40)]:
        a = load(p)
        best = None
        for bn, b in Bi:
            pct, mx, nd = diff(a, b)
            if best is None or pct < best[0]:
                best = (pct, mx, nd, bn)
        worst = max(worst, best[0])
        print("  A f_%s -> B f_%s  %7.3f%% max=%d px=%d"
              % (n, best[3], best[0], best[1], best[2]))
    print("\nworst best-match %%.3f%%" % worst if False else
          "\nworst best-match = %.3f %%" % worst)


if __name__ == "__main__":
    main()
