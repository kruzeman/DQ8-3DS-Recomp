"""What transform does the guest's composite pass apply to unit 0?

Reads the DQ8_RS_SNAP_DUMP trio (fbo_N = the target BEFORE the draw that
sampled it, post_N = the same target AFTER that draw) and reports mean |d| for
identity / mirror-x / mirror-y / rot-180 / transpose / rot-90, on the full
frame and on the target's own non-black box.  If the pass is a 1:1 blit the
identity entry is the small one; if the pass maps the source with swapped axes
the transpose/rot entries win -- that is what makes a source-side y flip show
up as an x mirror in the output.

usage: cmp_prepost.py <dumpdir> [n]
"""
import sys
from PIL import Image, ImageChops, ImageStat


def d(a, b):
    return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 4.0


def main():
    ddir = sys.argv[1].rstrip('/')
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    dims = open('%s/dims_%d.txt' % (ddir, n)).read().split()
    tw, th = int(dims[4]), int(dims[5])
    pre = Image.frombytes('RGBA', (tw, th),
                          open('%s/fbo_%d.raw' % (ddir, n), 'rb').read()[:tw * th * 4])
    post = Image.frombytes('RGBA', (tw, th),
                           open('%s/post_%d.raw' % (ddir, n), 'rb').read()[:tw * th * 4])
    cands = {
        'identity': post,
        'mirror-x': post.transpose(Image.FLIP_LEFT_RIGHT),
        'mirror-y': post.transpose(Image.FLIP_TOP_BOTTOM),
        'rot-180': post.transpose(Image.ROTATE_180),
        'transpose': post.transpose(Image.TRANSPOSE),
        'rot-90': post.transpose(Image.ROTATE_90),
    }
    print('draw %d  target %dx%d  (pre = before, post = after)' % (n, tw, th))
    for name, im in cands.items():
        print('  %-10s mean|d| = %7.3f' % (name, d(pre, im)))
    bbox = pre.convert('L').getbbox()
    if bbox:
        print('  pre non-black bbox %s' % (bbox,))


if __name__ == '__main__':
    main()
