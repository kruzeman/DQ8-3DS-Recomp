#!/usr/bin/env python3
"""t_8df70ad4: pixel-diff two P6 PPMs (400x240 frames).

usage: zz_ppmdiff.py a.ppm b.ppm
prints differing pixel count, the % of the screen, mean |delta| and the bbox.
"""
import sys


def read_ppm(path):
    d = open(path, 'rb').read()
    if not d.startswith(b'P6'):
        raise SystemExit('not P6: ' + path)
    i, toks = 2, []
    while len(toks) < 3:
        while d[i:i + 1].isspace():
            i += 1
        if d[i:i + 1] == b'#':
            while d[i:i + 1] != b'\n':
                i += 1
            continue
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        toks.append(int(d[i:j]))
        i = j
    i += 1
    w, h, _ = toks
    return w, h, d[i:]


a, b = sys.argv[1], sys.argv[2]
wa, ha, pa = read_ppm(a)
wb, hb, pb = read_ppm(b)
if (wa, ha) != (wb, hb):
    raise SystemExit('size mismatch %dx%d vs %dx%d' % (wa, ha, wb, hb))
n = wa * ha
ndiff = 0
sumd = 0
x0, y0, x1, y1 = 1 << 30, 1 << 30, -1, -1
for k in range(n):
    o = k * 3
    d0 = abs(pa[o] - pb[o])
    d1 = abs(pa[o + 1] - pb[o + 1])
    d2 = abs(pa[o + 2] - pb[o + 2])
    if d0 or d1 or d2:
        ndiff += 1
        sumd += d0 + d1 + d2
        x, y = k % wa, k // wa
        if x < x0: x0 = x
        if x > x1: x1 = x
        if y < y0: y0 = y
        if y > y1: y1 = y
print('%s vs %s: %dx%d differing=%d (%.2f%% of pixels) mean_delta=%.3f bbox=%s'
      % (a.split('/')[-2], b.split('/')[-2], wa, ha, ndiff,
         100.0 * ndiff / n, (sumd / (3.0 * ndiff)) if ndiff else 0.0,
         ('%d,%d..%d,%d' % (x0, y0, x1, y1)) if ndiff else '-'))
