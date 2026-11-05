"""Compare two raw RGBA images of the same size under identity / mirror-x /
mirror-y / rot-180 (box-downscaled by `f` first when asked).

usage: cmp_raw.py A.raw WxH B.raw [--box F]
"""
import sys
from PIL import Image, ImageChops, ImageStat


def load(path, dims):
    w, h = (int(v) for v in dims.split('x'))
    return Image.frombytes('RGBA', (w, h), open(path, 'rb').read()[:w * h * 4])


a = load(sys.argv[1], sys.argv[2])
b = load(sys.argv[3], sys.argv[2])
box = 1
if '--box' in sys.argv:
    box = int(sys.argv[sys.argv.index('--box') + 1])
if box > 1:
    a = a.resize((a.width // box, a.height // box), Image.BOX)
    b = b.resize((b.width // box, b.height // box), Image.BOX)
cands = {
    'identity': b,
    'mirror-x': b.transpose(Image.FLIP_LEFT_RIGHT),
    'mirror-y': b.transpose(Image.FLIP_TOP_BOTTOM),
    'rot-180': b.transpose(Image.ROTATE_180),
}
for name, im in cands.items():
    st = ImageStat.Stat(ImageChops.difference(a, im))
    print('%-9s mean|d| = %.3f  (per channel %s)'
          % (name, sum(st.mean) / 4.0, ' '.join('%.2f' % v for v in st.mean)))
