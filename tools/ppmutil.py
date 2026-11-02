#!/usr/bin/env python3
"""t_d559a510 helper: PPM frame utilities (convert / diff stats / region crop).

usage:
  ppmutil.py png  <in.ppm> <out.png>
  ppmutil.py diff <a.ppm> <b.ppm> [out_diff.png]
  ppmutil.py crop <in.ppm> <out.png> x0 y0 x1 y1
  ppmutil.py list <dir>
"""
import sys
import glob
import os


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    # P6 header
    assert data[:2] == b'P6', path
    vals = []
    i = 2
    while len(vals) < 3:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while data[i:i + 1] != b'\n':
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        vals.append(int(data[i:j]))
        i = j
    i += 1
    w, h, maxv = vals
    assert maxv == 255
    return w, h, data[i:i + w * h * 3]


def save_ppm(path, w, h, px):
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (w, h))
        f.write(px)


def to_png(w, h, px, out):
    from PIL import Image
    Image.frombytes('RGB', (w, h), px).save(out)


def main():
    cmd = sys.argv[1]
    if cmd == 'png':
        w, h, px = read_ppm(sys.argv[2])
        to_png(w, h, px, sys.argv[3])
    elif cmd == 'crop':
        w, h, px = read_ppm(sys.argv[2])
        x0, y0, x1, y1 = (int(v) for v in sys.argv[4:8])
        cw, ch = x1 - x0, y1 - y0
        out = bytearray()
        for y in range(y0, y1):
            off = (y * w + x0) * 3
            out += px[off:off + cw * 3]
        to_png(cw, ch, bytes(out), sys.argv[3])
    elif cmd == 'diff':
        wa, ha, pa = read_ppm(sys.argv[2])
        wb, hb, pb = read_ppm(sys.argv[3])
        assert (wa, ha) == (wb, hb), ((wa, ha), (wb, hb))
        n = wa * ha
        maxd = 0
        cnt = 0
        big = 0
        sumd = 0
        first = None
        # bounding box of differing pixels
        x0, y0, x1, y1 = wa, ha, -1, -1
        for i in range(n):
            d = 0
            for k in range(3):
                a = pa[i * 3 + k]
                b = pb[i * 3 + k]
                dd = a - b if a > b else b - a
                if dd > d:
                    d = dd
            if d:
                cnt += 1
                sumd += d
                if d > maxd:
                    maxd = d
                if d > 8:
                    big += 1
                x, y = i % wa, i // wa
                if first is None:
                    first = (x, y, pa[i * 3:i * 3 + 3], pb[i * 3:i * 3 + 3])
                if x < x0:
                    x0 = x
                if y < y0:
                    y0 = y
                if x > x1:
                    x1 = x
                if y > y1:
                    y1 = y
        print('size=%dx%d diff_px=%d (%.3f%%) maxd=%d mean_d=%.2f big(>8)=%d'
              % (wa, ha, cnt, 100.0 * cnt / n, maxd,
                 (sumd / cnt) if cnt else 0.0, big))
        if first:
            print('first_diff=%r bbox=%d,%d..%d,%d' % (first, x0, y0, x1, y1))
        if len(sys.argv) > 4:
            out = bytearray()
            for i in range(n):
                d = max(abs(pa[i * 3 + k] - pb[i * 3 + k]) for k in range(3))
                v = min(255, d * 4)
                out += bytes((v, v, v))
            to_png(wa, ha, bytes(out), sys.argv[4])
    elif cmd == 'list':
        for f in sorted(glob.glob(os.path.join(sys.argv[2], 'f_*.ppm'))):
            print(f, os.path.getsize(f))
    else:
        raise SystemExit(__doc__)


main()
