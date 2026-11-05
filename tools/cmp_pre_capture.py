"""Relate a pre-composite target image (2048x1600 raw or 2048x2048 snap dump)
to the native 400x240 captured screen, under the four axis transforms.

usage: cmp_pre_capture.py {raw:WxH:path | snap:path} screen.ppm [scale] [FIT]
"""
import sys
from PIL import Image, ImageChops, ImageStat


def load_pre(spec):
    if spec.startswith('snap:'):
        im = Image.frombytes('RGBA', (2048, 2048), open(spec[5:], 'rb').read()[:2048 * 2048 * 4])
        return im.crop((0, 2048 - 1600, 2048, 2048))
    _, dims, path = spec.split(':', 2)
    w, h = (int(v) for v in dims.split('x'))
    return Image.frombytes('RGBA', (w, h), open(path, 'rb').read()[:w * h * 4])


def load_ppm(p):
    data = open(p, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return Image.frombytes('RGB', (w, h), parts[3][:w * h * 3])


pre = load_pre(sys.argv[1])
cap = load_ppm(sys.argv[2])
f = int(sys.argv[3]) if len(sys.argv) > 3 else 4
pre = pre.resize((pre.width // f, pre.height // f), Image.BOX)
if len(sys.argv) > 4 and sys.argv[4] == 'FIT':
    # the visible screen is a crop of the target; try the left-aligned
    # 480x400 window scaled to the capture, which is what the display
    # transfer's numbers in the log imply
    pre = pre.crop((0, 0, 480, 400))
pre = pre.convert('RGB').resize(cap.size, Image.BOX)
for name, im in (('identity', cap),
                 ('mirror-x', cap.transpose(Image.FLIP_LEFT_RIGHT)),
                 ('mirror-y', cap.transpose(Image.FLIP_TOP_BOTTOM)),
                 ('rot-180', cap.transpose(Image.ROTATE_180))):
    st = ImageStat.Stat(ImageChops.difference(pre, im))
    print('%-9s mean|d| = %.3f' % (name, sum(st.mean) / 3.0))
