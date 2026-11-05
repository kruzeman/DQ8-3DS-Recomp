"""Compare the snapshot's target window (snap_N.raw rows 448..2048) against a
plain 2048x1600 RGBA target read-back, under the four axis transforms.

usage: cmp_snap_fbo.py <snap.raw> <fbo.raw> [scale]
"""
import sys
from PIL import Image, ImageChops, ImageStat


def main():
    snap = Image.frombytes('RGBA', (2048, 2048), open(sys.argv[1], 'rb').read()[:2048 * 2048 * 4])
    fbo = Image.frombytes('RGBA', (2048, 1600), open(sys.argv[2], 'rb').read()[:2048 * 1600 * 4])
    f = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    win = snap.crop((0, 2048 - 1600, 2048, 2048))
    a = win.resize((2048 // f, 1600 // f), Image.BOX)
    b = fbo.resize((2048 // f, 1600 // f), Image.BOX)
    for name, im in (('identity', b),
                     ('mirror-x', b.transpose(Image.FLIP_LEFT_RIGHT)),
                     ('mirror-y', b.transpose(Image.FLIP_TOP_BOTTOM)),
                     ('rot-180', b.transpose(Image.ROTATE_180))):
        st = ImageStat.Stat(ImageChops.difference(a, im))
        print('%-9s mean|d| = %.3f  (%s)' % (name, sum(st.mean) / 4.0,
                                             ' '.join('%.2f' % v for v in st.mean)))


if __name__ == '__main__':
    main()
