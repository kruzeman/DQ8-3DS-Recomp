import sys, re
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
w, h = im.size
print('imgsize', w, h)
px = im.tobytes()
# nonblack + white bbox
nb = 0; wx = []
for i in range(0, len(px), 3):
    r, g, b = px[i], px[i+1], px[i+2]
    if r > 15 or g > 15 or b > 15:
        nb += 1
        x = (i//3) % w
        y = (i//3) // w
        if r > 200 and g > 200 and b > 200:
            wx.append((x, y))
print('nonblack', nb, '/', w*h, 'white', len(wx))
if wx:
    xs = [p[0] for p in wx]; ys = [p[1] for p in wx]
    print('white bbox x', min(xs), max(xs), 'y', min(ys), max(ys))
    # column profile: count white per column
    cols = {}
    for x, y in wx:
        cols[x] = cols.get(x, 0) + 1
    bands = []
    prev = False
    cur = []
    for x in range(w):
        if cols.get(x, 0) > 0 and not prev:
            cur = [x]; prev = True
        elif cols.get(x, 0) == 0 and prev:
            bands.append((cur[0], x-1, sum(cols[c] for c in range(cur[0], x)))); prev = False
    if prev:
        bands.append((cur[0], w-1, sum(cols[c] for c in range(cur[0], w))))
    print('white vertical bands (x0,x1,count):')
    for b in bands:
        print(' ', b)
    # row profile
    rows = {}
    for x, y in wx:
        rows[y] = rows.get(y, 0) + 1
    rb = []
    prev = False; cur = []
    for y in range(h):
        if rows.get(y,0) > 0 and not prev:
            cur = [y]; prev = True
        elif rows.get(y,0) == 0 and prev:
            rb.append((cur[0], y-1, sum(rows[r] for r in range(cur[0], y)))) ; prev = False
    if prev:
        rb.append((cur[0], h-1, sum(rows[r] for r in range(cur[0], h))))
    print('white horizontal bands (y0,y1,count):')
    for b in rb:
        print(' ', b)
