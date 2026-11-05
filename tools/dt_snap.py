"""Which transform relates the scaled-sample snapshot to its source target?

Reads the files written by DQ8_RS_SNAP_DUMP=<dir> (see pica_gl.c
pgl_snap_diag): snap_N.raw (RGBA8, snap_w x snap_h), fbo_N.raw (RGBA8,
tgt_w x tgt_h) and dims_N.txt.  Both come straight out of glReadPixels, so
they share one row order; anything left is the blit's own transform.

For every placement (window offset in y) x x-flip x y-flip it reports the
mean |d| per channel, best first.  A near-zero entry names the transform the
copy actually performed.  Also reports where the target's own content sits
inside the snapshot (rows with any non-black pixel), which pins the y
placement without assuming it.

usage: dt_snap.py <dumpdir> [n]
"""
import sys
from PIL import Image, ImageChops, ImageStat


def load_rgba(path, w, h):
    data = open(path, 'rb').read()
    return Image.frombytes('RGBA', (w, h), data[:w * h * 4])


def main():
    d = sys.argv[1].rstrip('/')
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    dims = open('%s/dims_%d.txt' % (d, n)).read().split()
    sw, sh = int(dims[1]), int(dims[2])
    tw, th = int(dims[4]), int(dims[5])
    dw, dh = (int(v) for v in dims[7].split('x'))
    f = int(dims[9])
    snap = load_rgba('%s/snap_%d.raw' % (d, n), sw, sh)
    fbo = load_rgba('%s/fbo_%d.raw' % (d, n), tw, th)
    print('snap %dx%d  target %dx%d  declared %dx%d  scale %d'
          % (sw, sh, tw, th, dw, dh, f))

    # where does the target's content sit in the snapshot?  (non-black rows)
    bbox = snap.convert('L').getbbox()
    print('snapshot non-black bbox: %s' % (bbox,))

    ref = fbo.resize((tw // f, th // f), Image.BOX)
    rows = []
    for off in (0, sh - th):
        win = snap.crop((0, off, tw, off + th))
        for yflip in (0, 1):
            w2 = win.transpose(Image.FLIP_TOP_BOTTOM) if yflip else win
            for xflip in (0, 1):
                w3 = w2.transpose(Image.FLIP_LEFT_RIGHT) if xflip else w2
                small = w3.resize((tw // f, th // f), Image.BOX)
                st = ImageStat.Stat(ImageChops.difference(small, ref))
                rows.append((sum(st.mean) / 4.0, off, yflip, xflip))
    for d_, off, yflip, xflip in sorted(rows):
        print('mean|d| %7.3f   yoff=%-4d yflip=%d xflip=%d' % (d_, off, yflip, xflip))


if __name__ == '__main__':
    main()
