#!/usr/bin/env python3
"""Best-phase alignment of two frame sets, excluding the static restored frame.

usage: holes_align2.py <dirA> <dirB> [thresh]
Writes <dirA>/align_anim.png = A | B | (diff>thresh) mask.
"""
import glob
import os
import sys

from PIL import Image, ImageChops, ImageStat

STATIC = '3b188277ba4acefc632296d7d17dc5a1'


def main():
    da, db = sys.argv[1], sys.argv[2]
    thresh = int(sys.argv[3]) if len(sys.argv) > 3 else 24
    fa = [p for p in sorted(glob.glob(os.path.join(da, 'f_*.ppm')))
          if os.path.getsize(p) == 288015 and STATIC not in p]
    fb = [p for p in sorted(glob.glob(os.path.join(db, 'f_*.ppm')))
          if os.path.getsize(p) == 288015 and STATIC not in p]
    A = [Image.open(p).convert('RGB') for p in fa]
    B = [Image.open(p).convert('RGB') for p in fb]
    scores = []
    for i, a in enumerate(A):
        for j, b in enumerate(B):
            st = ImageStat.Stat(ImageChops.difference(a, b))
            scores.append((sum(st.mean) / 3.0, i, j))
    scores.sort()
    for s in scores[:5]:
        print("cand mean_abs=%.3f A=%s B=%s" %
              (s[0], os.path.basename(fa[s[1]]), os.path.basename(fb[s[2]])))
    d, i, j = scores[0]
    a, b = A[i], B[j]
    diff = ImageChops.difference(a, b).convert('L')
    px = diff.point(lambda v: 255 if v > thresh else 0)
    w, h = diff.size
    mask = px.load()
    n = 0
    x0, x1, y0, y1 = w, -1, h, -1
    for y in range(h):
        for x in range(w):
            if mask[x, y]:
                n += 1
                x0 = min(x0, x)
                x1 = max(x1, x)
                y0 = min(y0, y)
                y1 = max(y1, y)
    print("best mean_abs=%.3f  pixels diff>%d: %d bbox x=%d..%d y=%d..%d" %
          (d, thresh, n, x0, x1, y0, y1))
    heat = Image.merge('RGB', (px, px.point(lambda v: v // 4), Image.new('L', (w, h), 0)))
    out = Image.new('RGB', (w * 3, h))
    out.paste(a, (0, 0))
    out.paste(b, (w, 0))
    out.paste(heat, (w * 2, 0))
    p = os.path.join(da, 'align_anim.png')
    out.save(p)
    print("wrote", p)


main()
