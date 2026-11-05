#!/usr/bin/env python3
"""tools/ppm_flat.py -- largest flat-colour fraction of a captured screen.

    python3 tools/ppm_flat.py <file.ppm> [x0 y0 x1 y1]

Prints one line:

    file=... w=400 h=240 pixels=96000 top_colour=00ff00 count=1234 frac=1.29%

"The world is gone" shows up as one colour covering a large part of the top
screen (the operator measured 57.1 % on a broken town frame vs 0.3 % on a
healthy one).  Only the P6 (binary RGB) writer in the runtime is handled; the
optional box lets a caller restrict the measurement to the 3D viewport.
"""
import sys


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    if not data.startswith(b'P6'):
        raise SystemExit('not a P6 ppm: %s' % path)
    fields = []
    i = 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while i < len(data) and data[i:i + 1] != b'\n':
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _maxv = fields
    return w, h, data[i:]


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    path = argv[1]
    w, h, px = read_ppm(path)
    x0, y0, x1, y1 = 0, 0, w, h
    if len(argv) >= 6:
        x0, y0, x1, y1 = (int(v) for v in argv[2:6])
    hist = {}
    for y in range(max(0, y0), min(h, y1)):
        row = y * w * 3
        for x in range(max(0, x0), min(w, x1)):
            o = row + x * 3
            c = (px[o] << 16) | (px[o + 1] << 8) | px[o + 2]
            hist[c] = hist.get(c, 0) + 1
    total = sum(hist.values())
    top, cnt = max(hist.items(), key=lambda kv: kv[1]) if hist else (0, 0)
    print('file=%s w=%d h=%d box=%d,%d..%d,%d pixels=%d top_colour=%06x '
          'count=%d frac=%.2f%%' % (path, w, h, x0, y0, x1, y1, total, top,
                                    cnt, 100.0 * cnt / total if total else 0.0))


if __name__ == '__main__':
    main(sys.argv)
