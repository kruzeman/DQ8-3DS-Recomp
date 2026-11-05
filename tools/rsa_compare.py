"""t_05281b89 stage A: evidence tooling for the internal render scale.

Loads native PPM captures of the SAME deterministic guest frame at several
scales and answers the acceptance questions with numbers:

  * AA metric: hard 3-byte scanline steps (>=60) must DROP, partial blends
    (8..59) must RISE with the scale.
  * geometry shift: the best-alignment offset of the scaled frame against the
    native one must be (0,0).
  * x1 vs box(x2): the scaled frame must be, per pixel, the box filter of the
    supersampled image, so |box(x1') - xf| stays small everywhere except at
    edges (where xf legitimately carries extra coverage).
  * PNG crops + a 1x|2x|4x panel for a human.

usage: rsa_compare.py <outdir> <x1.ppm> <x2.ppm> <x4.ppm> [x0 y0 x1 y1]
"""
import sys
from PIL import Image


def load(p):
    data = open(p, 'rb').read()
    parts = data.split(b'\n', 3)
    assert parts[0].strip() == b'P6', p
    w, h = map(int, parts[1].split())
    return w, h, parts[3][:w * h * 3]


def aa(px, w, h):
    hard = partial = 0
    total = 0.0
    for y in range(h):
        row = y * w * 3
        for x in range(w - 1):
            i = row + x * 3
            g = (abs(px[i] - px[i + 3]) + abs(px[i + 1] - px[i + 4]) +
                 abs(px[i + 2] - px[i + 5]))
            total += g
            if g >= 60:
                hard += 1
            elif g >= 8:
                partial += 1
    return hard, partial, total / float(w * h)


def meand(a, b, w, h, dx=0, dy=0):
    tot = 0
    n = 0
    for y in range(2, h - 2):
        yy = y + dy
        if yy < 0 or yy >= h:
            continue
        for x in range(2, w - 2):
            xx = x + dx
            if xx < 0 or xx >= w:
                continue
            i = (y * w + x) * 3
            j = (yy * w + xx) * 3
            tot += (abs(a[i] - b[j]) + abs(a[i + 1] - b[j + 1]) +
                    abs(a[i + 2] - b[j + 2]))
            n += 1
    return tot / max(1, n)


def shift_scan(a, b, w, h, r=3):
    best = None
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            e = meand(a, b, w, h, dx, dy)
            if best is None or e < best[0]:
                best = (e, dx, dy)
    return best


def boxf(px, w, h, f):
    out = bytearray(len(px))
    half = f // 2
    for y in range(h):
        y0 = max(0, y - half)
        y1 = min(h, y0 + f)
        for x in range(w):
            x0 = max(0, x - half)
            x1 = min(w, x0 + f)
            acc = [0, 0, 0]
            c = 0
            for yy in range(y0, y1):
                base = (yy * w + x0) * 3
                for xx in range(x1 - x0):
                    acc[0] += px[base + xx * 3]
                    acc[1] += px[base + xx * 3 + 1]
                    acc[2] += px[base + xx * 3 + 2]
                    c += 1
            o = (y * w + x) * 3
            out[o] = acc[0] // c
            out[o + 1] = acc[1] // c
            out[o + 2] = acc[2] // c
    return bytes(out)


def main():
    out = sys.argv[1]
    paths = sys.argv[2:5]
    w, h, base = load(paths[0])
    frames = [base]
    for p in paths[1:]:
        w2, h2, px = load(p)
        assert (w, h) == (w2, h2), (w, h, w2, h2)
        frames.append(px)
    scales = [1, 2, 4]
    print('frame %dx%d' % (w, h))
    print('%-8s %8s %9s %12s' % ('scale', 'hard', 'partial', 'mean|grad|'))
    for s, px in zip(scales, frames):
        hard, partial, mg = aa(px, w, h)
        print('%-8s %8d %9d %12.3f' % ('x%d' % s, hard, partial, mg))
    for s, px in zip(scales[1:], frames[1:]):
        e, dx, dy = shift_scan(base, px, w, h)
        print('x%d vs x1 best alignment: (%+d,%+d) mean|d|=%.2f -> %s'
              % (s, dx, dy, e,
                 'no geometry shift' if (dx, dy) == (0, 0) else 'SHIFT!'))
        bb = boxf(base, w, h, s)
        big = 0
        for i in range(0, len(bb), 3):
            d = abs(bb[i] - px[i]) + abs(bb[i + 1] - px[i + 1]) + abs(bb[i + 2] - px[i + 2])
            if d > 30:
                big += 1
        print('x%d box%d(x1) vs x%d: mean|d|=%.2f, pixels off by >30: %d/%d'
              % (s, s, s, meand(bb, px, w, h), big, w * h))

    # PNGs + panel (nearest 2x so stair-steps are visible)
    ims = [Image.frombytes('RGB', (w, h), px) for px in frames]
    for s, im in zip(scales, ims):
        im.save('%s/frame_x%d.png' % (out, s))
    if len(sys.argv) > 5:
        bx = tuple(int(v) for v in sys.argv[5:9])
    else:
        bx = (w // 4, h // 4, w // 4 + w // 2, h // 4 + h // 2)
    zw = 3
    panel = Image.new('RGB', (len(ims) * w + (len(ims) - 1) * 6, h), (24, 24, 24))
    for i, im in enumerate(ims):
        panel.paste(im, (i * (w + 6), 0))
    panel.resize((panel.width * 2, panel.height * 2), Image.NEAREST).save(
        '%s/panel_x1x2x4.png' % out)
    crop = Image.new('RGB', (len(ims) * (bx[2] - bx[0]) + (len(ims) - 1) * 6,
                             bx[3] - bx[1]), (24, 24, 24))
    for i, im in enumerate(ims):
        crop.paste(im.crop(bx), (i * (bx[2] - bx[0] + 6), 0))
    crop.resize((crop.width * 4, crop.height * 4), Image.NEAREST).save(
        '%s/panel_crop_x1x2x4.png' % out)
    print('wrote %s/frame_x1..x4.png, panel_x1x2x4.png, panel_crop_x1x2x4.png'
          % out)


main()
