"""Build a labelled PNG montage from a list of PPM frames.

usage: python3 tools/montage_frames.py OUT.png COLS FRAME [FRAME ...]
       python3 tools/montage_frames.py OUT.png --dir DIR [--pattern '*.ppm'] [--cols N] [--scale N]

Frames are validated (full P6 payload) before use; a truncated frame raises.
Labels come from the tNNNN time stamp in the file name, prefixed with the
parent directory name when it is not the usual 'frames' capture directory (so
two runs can be laid out in one sheet).
"""
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw

HDR = re.compile(rb'P6\s+(\d+)\s+(\d+)\s+255\s')


def load(path):
    data = Path(path).read_bytes()
    m = HDR.match(data)
    if not m:
        raise ValueError(f'{path}: not a P6 ppm')
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():]
    if len(px) < w * h * 3:
        raise ValueError(f'{path}: truncated {len(px)} < {w * h * 3}')
    return Image.frombytes('RGB', (w, h), px[:w * h * 3])


def label_for(path):
    m = re.search(r't(\d{4})', path.stem)
    when = f'{int(m.group(1))}s' if m else path.stem
    parent = path.parent.name
    return when if parent in ('', 'frames') else f'{parent}:{when}'


def main(argv):
    out = argv[1]
    args = list(argv[2:])
    cols = 6
    scale = 1
    frames = []

    if '--scale' in args:
        i = args.index('--scale')
        scale = int(args[i + 1])
        del args[i:i + 2]
    if '--cols' in args:
        i = args.index('--cols')
        cols = int(args[i + 1])
        del args[i:i + 2]
    if '--pattern' in args:
        i = args.index('--pattern')
        pattern = args[i + 1]
        del args[i:i + 2]
    else:
        pattern = '*.ppm'

    if args and args[0] == '--dir':
        frames = sorted(Path(args[1]).glob(pattern))
    else:
        frames = [Path(a) for a in args]

    if not frames:
        print('no frames', file=sys.stderr)
        return 1

    imgs, labels = [], []
    for f in frames:
        imgs.append(load(f))
        labels.append(label_for(f))

    w, h = imgs[0].size
    w, h = w * scale, h * scale
    badge = 16
    rows = (len(imgs) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * w, rows * (h + badge)), (32, 32, 32))
    draw = ImageDraw.Draw(sheet)
    for i, (im, lab) in enumerate(zip(imgs, labels)):
        x = (i % cols) * w
        y = (i // cols) * (h + badge)
        sheet.paste(im.resize((w, h)), (x, y))
        draw.text((x + 3, y + h + 2), lab, fill=(255, 255, 0))
    sheet.save(out)
    print(f'saved {out} {sheet.size} from {len(imgs)} frames')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
