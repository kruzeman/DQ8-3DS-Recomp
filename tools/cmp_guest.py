"""Decode two guest colour buffers (PICA 8x8 Morton, 4 B/px stored A,B,G,R)
and compare them under identity / mirror-x / mirror-y / rot-180.

usage: cmp_guest.py A.raw B.raw W H
"""
import sys
from PIL import Image, ImageChops, ImageStat

XL = (0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15)
YL = (0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a)


def decode(path, w, h):
    raw = open(path, 'rb').read()
    out = bytearray(w * h * 3)
    row = (w >> 3) * 64
    for gy in range(h):
        ty = (gy >> 3) * row
        yy = YL[gy & 7]
        for gx in range(w):
            off = (ty + (gx & ~7) * 8 + XL[gx & 7] + yy) * 4
            o = (gy * w + gx) * 3
            out[o] = raw[off + 3]
            out[o + 1] = raw[off + 2]
            out[o + 2] = raw[off + 1]
    return Image.frombytes('RGB', (w, h), bytes(out))


a = decode(sys.argv[1], int(sys.argv[3]), int(sys.argv[4]))
b = decode(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
for name, im in (('identity', b),
                 ('mirror-x', b.transpose(Image.FLIP_LEFT_RIGHT)),
                 ('mirror-y', b.transpose(Image.FLIP_TOP_BOTTOM)),
                 ('rot-180', b.transpose(Image.ROTATE_180))):
    st = ImageStat.Stat(ImageChops.difference(a, im))
    print('%-9s mean|d| = %.3f  (%s)' % (name, sum(st.mean) / 3.0,
                                         ' '.join('%.2f' % v for v in st.mean)))
