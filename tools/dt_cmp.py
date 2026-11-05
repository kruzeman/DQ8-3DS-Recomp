"""t_dd830a74: 1x vs 4x comparison for the scaled-sample (Azahar) path.

Loads the native PPM captures of the same deterministic guest frame at 1x and
4x, then:
  * saves both as PNG for a human,
  * downscales the 4x capture to native (box) and reports the residual against
    the 1x capture: mean |d|, share off by >8/>30, best alignment shift,
  * writes a side-by-side PNG and a heat map of the residual.

usage: dt_cmp.py <outdir> <x1.ppm> <x4.ppm>
"""
import sys
from PIL import Image


def load(p):
    data = open(p, 'rb').read()
    parts = data.split(b'\n', 3)
    assert parts[0].strip() == b'P6', p
    w, h = map(int, parts[1].split())
    return Image.frombytes('RGB', (w, h), parts[3][:w * h * 3])


def main():
    out, p1, p4 = sys.argv[1], sys.argv[2], sys.argv[3]
    a = load(p1)
    b = load(p4)
    w, h = a.size
    print('native %dx%d, x4 capture %dx%d' % (w, h, b.size[0], b.size[1]))
    b.resize((w, h), Image.BOX).save(out + '/x4_boxed.png')
    a.save(out + '/x1.png')
    b.save(out + '/x4.png')
    box = b.resize((w, h), Image.BOX).load()
    pa = a.load()
    tot = 0
    n8 = n30 = 0
    heat = Image.new('RGB', (w, h))
    hp = heat.load()
    for y in range(h):
        for x in range(w):
            r = pa[x, y]
            q = box[x, y]
            d = (abs(r[0] - q[0]) + abs(r[1] - q[1]) + abs(r[2] - q[2])) / 3.0
            tot += d
            if d > 8:
                n8 += 1
            if d > 30:
                n30 += 1
            v = min(255, int(d * 3))
            hp[x, y] = (v, v // 2, 0)
    n = w * h
    print('residual box4(x4) vs x1: mean|d|=%.2f  >8: %d/%d (%.2f%%)  >30: %d/%d (%.2f%%)'
          % (tot / n, n8, n, 100.0 * n8 / n, n30, n, 100.0 * n30 / n))
    # best alignment: shift the boxed 4x by +-4 px and report the best mean |d|
    best = None
    for sy in range(-4, 5):
        for sx in range(-4, 5):
            s = 0
            c = 0
            for y in range(4, h - 4, 2):
                for x in range(4, w - 4, 2):
                    r = pa[x, y]
                    q = box[x + sx, y + sy]
                    s += abs(r[0] - q[0]) + abs(r[1] - q[1]) + abs(r[2] - q[2])
                    c += 3
            v = s / float(c)
            if best is None or v < best[0]:
                best = (v, sx, sy)
    print('best alignment of box4(x4) against x1: (%+d,%+d) mean|d|=%.2f'
          % (best[1], best[2], best[0]))
    heat.save(out + '/residual.png')
    side = Image.new('RGB', (w * 2 + 4, h))
    side.paste(a, (0, 0))
    side.paste(b.resize((w, h), Image.BOX), (w + 4, 0))
    side.save(out + '/side_x1_box4.png')


main()
