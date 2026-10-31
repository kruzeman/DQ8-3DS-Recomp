import sys, re
from pathlib import Path
p = sys.argv[1]
data = Path(p).read_bytes()
m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', data)
w, h = int(m.group(1)), int(m.group(2))
px = data[m.end():]
nb = sum(1 for i in range(0, len(px), 3) if px[i] > 15 or px[i+1] > 15 or px[i+2] > 15)
print('dims', w, h, 'nonblack', nb, '/', w*h)
xs, ys = [], []
for i in range(0, len(px), 3):
    if px[i] > 200 and px[i+1] > 200 and px[i+2] > 200:
        x = (i//3) % w
        y = (i//3) // w
        xs.append(x)
        ys.append(y)
print('white px', len(xs), 'bbox x', (min(xs) if xs else -1), (max(xs) if xs else -1),
      'y', (min(ys) if xs else -1), (max(ys) if xs else -1))
for y in (40, 80, 120, 160, 200, 239):
    row = y * 3 * w
    cols = [c for c in range(w) if px[row + c*3] > 15 or px[row + c*3 + 1] > 15 or px[row + c*3 + 2] > 15]
    print('row', y, 'nonblack cols', (cols[0] if cols else -1), 'to', (cols[-1] if cols else -1), 'count', len(cols))
try:
    from PIL import Image
    im = Image.frombytes('RGB', (w, h), px)
    out = p + '.png'
    im.save(out)
    print('saved', out)
except Exception as e:
    print('PIL fail', e)
