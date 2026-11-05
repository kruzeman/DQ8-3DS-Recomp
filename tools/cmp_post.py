"""The composite's input -> output transform: compare the pre-draw snapshot
(snap_N.raw, target window at rows snap_h-tgt_h .. snap_h) with the target as
GL leaves it after that draw (post_N.raw).

usage: cmp_post.py <dumpdir> [n] [scale]
"""
import sys
from PIL import Image, ImageChops, ImageStat


def main():
    d = sys.argv[1].rstrip('/')
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    f = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    snap = Image.frombytes('RGBA', (2048, 2048),
                           open('%s/snap_%d.raw' % (d, n), 'rb').read()[:2048 * 2048 * 4])
    post = Image.frombytes('RGBA', (2048, 1600),
                           open('%s/post_%d.raw' % (d, n), 'rb').read()[:2048 * 1600 * 4])
    pre = snap.crop((0, 2048 - 1600, 2048, 2048))
    a = post.resize((2048 // f, 1600 // f), Image.BOX)
    b = pre.resize((2048 // f, 1600 // f), Image.BOX)
    for name, im in (('identity', b),
                     ('mirror-x', b.transpose(Image.FLIP_LEFT_RIGHT)),
                     ('mirror-y', b.transpose(Image.FLIP_TOP_BOTTOM)),
                     ('rot-180', b.transpose(Image.ROTATE_180))):
        st = ImageStat.Stat(ImageChops.difference(a, im))
        print('pre->post %-9s mean|d| = %.3f  (%s)'
              % (name, sum(st.mean) / 4.0, ' '.join('%.2f' % v for v in st.mean)))


if __name__ == '__main__':
    main()
