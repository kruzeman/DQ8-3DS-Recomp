"""Compare the guest render target bytes against the GL FBO read-back.

Reads the files DQ8_GL_DUMP_DIR writes at a flush:
  color_fbo_last.raw    RGBA8, w*h from color_fbo_last.box, glReadPixels order
                        (row 0 = GL row y0, i.e. the BOTTOM of GL space)
  color_guest_last.raw  the guest colour buffer, PICA 8x8 Morton tiles, 4 B/px
                        stored A,B,G,R (rt_w x rt_h from the box file)
  color_fbo_last.box    "x0 y0 x1 y1 rt_w rt_h"

Decodes the guest buffer with the tiling pica_rt_off uses and reports
mean |d| of the two images under identity / mirror-x / mirror-y / rot-180,
after mapping the GL row order to the guest row order.  This is the
measurement behind "FBO column = guest column" (the claim the snapshot
orientation rests on) — code reading is not enough.

usage: dt_wb.py <dumpdir>
"""
import sys
from PIL import Image, ImageChops, ImageStat

XL = (0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15)
YL = (0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a)


def main():
    d = sys.argv[1].rstrip('/')
    x0, y0, x1, y1, rw, rh = (int(v) for v in open(d + '/color_fbo_last.box').read().split())
    w, h = x1 - x0 + 1, y1 - y0 + 1
    fbo = Image.frombytes('RGBA', (w, h), open(d + '/color_fbo_last.raw', 'rb').read()[:w * h * 4]).convert('RGB')
    raw = open(d + '/color_guest_last.raw', 'rb').read()
    print('box %d,%d..%d,%d  rt %dx%d  fbo %dx%d  guest bytes %d'
          % (x0, y0, x1, y1, rw, rh, w, h, len(raw)))
    assert len(raw) >= rw * rh * 4
    # decode guest memory -> top-down RGB image, A,B,G,R stored order
    guest = bytearray(rw * rh * 3)
    for gy in range(rh):
        for gx in range(rw):
            off = (((gy >> 3) * (rw >> 3) * 64) + (gx & ~7 & 0xFFFF) * 8 +
                   XL[gx & 7] + YL[gy & 7]) * 4
            o = (gy * rw + gx) * 3
            guest[o] = raw[off + 3]
            guest[o + 1] = raw[off + 2]
            guest[o + 2] = raw[off + 1]
    gimg = Image.frombytes('RGB', (rw, rh), bytes(guest))
    # GL row (y0+j) is guest row (rh-1-(y0+j)); the box is a contiguous run
    crop = gimg.crop((x0, rh - 1 - y1, x1 + 1, rh - 1 - y0 + 1))
    print('guest crop rows %d..%d' % (rh - 1 - y1, rh - 1 - y0))
    cands = {
        'identity': fbo,
        'mirror-x': fbo.transpose(Image.FLIP_LEFT_RIGHT),
        'mirror-y': fbo.transpose(Image.FLIP_TOP_BOTTOM),
        'rot-180': fbo.transpose(Image.ROTATE_180),
    }
    for name, im in cands.items():
        st = ImageStat.Stat(ImageChops.difference(crop, im))
        print('%-9s mean|d| = %.3f' % (name, sum(st.mean) / 3.0))


if __name__ == '__main__':
    main()
