#!/usr/bin/env python3
"""t_8df70ad4: visual diff — write a PPM that shows A normally, B's differing
pixels (|d| > thr on any channel) painted magenta.

usage: zz_ppmdiffimg.py a.ppm b.ppm out.ppm [thr]
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


a, b, out = sys.argv[1], sys.argv[2], sys.argv[3]
thr = int(sys.argv[4]) if len(sys.argv) > 4 else 16
w, h, pa = read_ppm(a)
_, _, pb = read_ppm(b)
px = bytearray(pa[:w * h * 3])
nd = 0
for k in range(w * h):
    o = k * 3
    if (abs(pa[o] - pb[o]) > thr or abs(pa[o + 1] - pb[o + 1]) > thr or
            abs(pa[o + 2] - pb[o + 2]) > thr):
        px[o] = 255
        px[o + 1] = 0
        px[o + 2] = 255
        nd += 1
open(out, 'wb').write(b'P6\n%d %d\n255\n' % (w, h) + bytes(px))
print('wrote %s: %d px marked (>%d on any channel)' % (out, nd, thr))
