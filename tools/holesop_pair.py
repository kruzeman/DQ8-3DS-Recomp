#!/usr/bin/env python3
"""t_a22413f1: compare two scene runs captured at wall-clock cadence.

The GL and SW runs are not phase-locked (different host speed), so an exact
content match is rare.  For every GL frame this finds the closest SW frame by
mean |delta| and reports the residual STRUCTURE of the best pairs: pixels over
threshold, and whether those pixels are in a few large blobs (a missing/changed
surface = a real renderer disagreement) or spread thin (animation phase / AA).

usage: holesop_pair.py <gl-dir> <sw-dir> [top_n] [out.png]
"""
import sys, os, glob
import numpy as np
from PIL import Image


def frames(d):
    fs = sorted(f for f in glob.glob(os.path.join(d, "[ts]0*.ppm")) if ".bot." not in f
                and os.path.getsize(f) >= 288015)
    return [(os.path.basename(f), np.asarray(Image.open(f).convert("RGB")).astype(np.int16))
            for f in fs]


def components(mask):
    """4-connected component sizes via union-find on a boolean mask."""
    h, w = mask.shape
    lab = -np.ones((h, w), np.int32)
    parent = []

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    n = 0
    for y in range(h):
        row = mask[y]
        for x in np.nonzero(row)[0]:
            up = lab[y - 1, x] if y else -1
            lf = lab[y, x - 1] if x else -1
            if up < 0 and lf < 0:
                lab[y, x] = n
                parent.append(n)
                n += 1
            elif up >= 0 and lf < 0:
                lab[y, x] = up
            elif up < 0 and lf >= 0:
                lab[y, x] = lf
            else:
                ru, rl = find(up), find(lf)
                lab[y, x] = rl
                if ru != rl:
                    parent[ru] = rl
    if not n:
        return []
    sizes = {}
    for y in range(h):
        for x in np.nonzero(mask[y])[0]:
            r = find(lab[y, x])
            sizes[r] = sizes.get(r, 0) + 1
    return sorted(sizes.values(), reverse=True)


def main():
    gl, sw = frames(sys.argv[1]), frames(sys.argv[2])
    step = int(os.environ.get("HOLES_STEP", "1"))
    gl = gl[::step]
    top_n = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    print("GL frames %d   SW frames %d" % (len(gl), len(sw)))
    cand = []
    for gn, gi in gl:
        for sn, si in sw:
            d = np.abs(gi - si).mean()
            cand.append((d, gn, sn))
    cand.sort()
    seen = set()
    picked = []
    for d, gn, sn in cand:
        if gn in seen:
            continue
        seen.add(gn)
        picked.append((d, gn, sn))
        if len(picked) >= top_n:
            break
    gd = dict(gl)
    sd = dict(sw)
    best = None
    for d, gn, sn in picked:
        a, b = gd[gn], sd[sn]
        if a.shape != b.shape:
            continue
        diff = np.abs(a - b).max(axis=2)
        m30 = diff > 30
        comps = components(m30)
        big = comps[:3]
        print("  GL %s vs SW %s : mean|d|=%.3f  px>30=%d  components=%d largest=%s"
              % (gn, sn, d, int(m30.sum()), len(comps), big))
        if best is None or d < best[0]:
            best = (d, gn, sn, a, b, m30)
    if best and len(sys.argv) > 4:
        d, gn, sn, a, b, m30 = best
        h, w, _ = a.shape
        canvas = np.zeros((h, w * 3 + 8, 3), np.uint8)
        canvas[:, :w] = np.clip(a, 0, 255).astype(np.uint8)
        canvas[:, w + 4:2 * w + 4] = np.clip(b, 0, 255).astype(np.uint8)
        canvas[:, 2 * w + 8:] = np.repeat(
            np.where(m30, 255, 0).astype(np.uint8)[:, :, None], 3, axis=2)
        Image.fromarray(canvas).resize((canvas.shape[1] * 2, canvas.shape[0] * 2),
                                      Image.NEAREST).save(sys.argv[4])
        print("wrote %s (best pair GL %s | SW %s | mask>30)" % (sys.argv[4], gn, sn))


main()
