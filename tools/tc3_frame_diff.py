#!/usr/bin/env python3
"""t_66c89cf2: localize the BEFORE/AFTER frame difference.

Writes a heatmap (red = differing pixels, dim gray = context) and reports
whether differences concentrate in animated regions (water plane, particles)
or in texture regions (which would indicate a caching bug)."""
import sys

def readppm(p):
    f = open(p, 'rb')
    assert f.readline().strip() == b'P6', p
    l = f.readline()
    while l.startswith(b'#'):
        l = f.readline()
    w, h = map(int, l.split())
    mx = int(f.readline())
    return w, h, f.read()

def main(pa, pb, out):
    w, h, da = readppm(pa)
    w2, h2, db = readppm(pb)
    assert (w, h) == (w2, h2), 'size mismatch'
    buf = bytearray(w * h * 3)
    diff_rows = {}
    total = 0
    for y in range(h):
        base = y * w * 3
        cnt = 0
        for x in range(w):
            i = base + x * 3
            dd = abs(da[i] - db[i]) + abs(da[i+1] - db[i+1]) + abs(da[i+2] - db[i+2])
            if dd > 24:
                buf[i] = 255
                buf[i+1] = 40
                buf[i+2] = 40
                cnt += 1
            else:
                g = (da[i] + da[i+1] + da[i+2]) // 6
                buf[i] = buf[i+1] = buf[i+2] = g
        if cnt:
            diff_rows[y] = cnt
            total += cnt
    open(out, 'wb').write(b'P6\n%d %d\n255\n' % (w, h) + bytes(buf))
    print('diff pixels: %d of %d = %.2f%%' % (total, w * h, 100.0 * total / (w * h)))
    top = sorted(diff_rows.items(), key=lambda kv: -kv[1])[:8]
    print('top rows (y:count):', top)
    band = h // 6
    for b in range(6):
        s = sum(v for y, v in diff_rows.items() if b * band <= y < (b + 1) * band)
        print('band y[%d,%d): %d diff px' % (b * band, (b + 1) * band, s))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3])
