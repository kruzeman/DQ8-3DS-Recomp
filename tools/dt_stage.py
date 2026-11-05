"""Compare the CPU-staged texture image (native path) against the GL target.

Files from DQ8_RS_SNAP_DUMP=<dir> with pgl_diag_stage_dump():
  staged_N.raw  512x512 RGBA, row r = what the sampler returns for v=(r+.5)/h,
                column c = s=(c+.5)/w (pica_vs.c pica_gl_stage_texture)
  sfbo_N.raw    the target at full internal scale, glReadPixels order
  sdims_N.txt   "staged W H tgt W H scale F"

The staged image is v-flipped vs the FBO *and* offset: staged row r holds the
target texel whose GL row is r - (declared_h - tgt_h), because the target only
occupies the bottom tgt_h rows of the declared height.  This tool finds the
placement and the x transform by measurement, so "the native path samples the
target unmirrored" is a fact rather than an assumption.

usage: dt_stage.py <dumpdir> [n]
"""
import sys
from PIL import Image, ImageChops, ImageStat


def main():
    d = sys.argv[1].rstrip('/')
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    toks = open('%s/sdims_%d.txt' % (d, n)).read().split()
    sw, sh = int(toks[1]), int(toks[2])
    tw, th = int(toks[4]), int(toks[5])
    f = int(toks[7])
    staged = Image.frombytes('RGBA', (sw, sh),
                             open('%s/staged_%d.raw' % (d, n), 'rb').read()[:sw * sh * 4])
    big = Image.frombytes('RGBA', (tw, th),
                          open('%s/sfbo_%d.raw' % (d, n), 'rb').read()[:tw * th * 4])
    ref_full = big.resize((tw // f, th // f), Image.BOX)      # GL row k -> row k
    nw, nh = tw // f, th // f
    print('staged %dx%d  target %dx%d (native %dx%d)  scale %d'
          % (sw, sh, tw, th, nw, nh, f))
    rows = []
    for off in (sh - nh, 0):
        win = staged.crop((0, off, nw, off + nh))
        for yflip in (0, 1):
            w2 = win.transpose(Image.FLIP_TOP_BOTTOM) if yflip else win
            for xflip in (0, 1):
                w3 = w2.transpose(Image.FLIP_LEFT_RIGHT) if xflip else w2
                st = ImageStat.Stat(ImageChops.difference(w3, ref_full))
                rows.append((sum(st.mean) / 4.0, off, yflip, xflip))
    for d_, off, yflip, xflip in sorted(rows):
        print('mean|d| %7.3f   yoff=%-4d yflip=%d xflip=%d' % (d_, off, yflip, xflip))


if __name__ == '__main__':
    main()
