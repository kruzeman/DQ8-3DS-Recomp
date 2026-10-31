import re, sys
data = open(sys.argv[1], 'rb').read()
m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', data)
w, h = int(m.group(1)), int(m.group(2))
px = data[m.end():]
for y in range(0, h, 8):
    row = y * 3 * w
    cols = [c for c in range(w) if px[row + c*3] > 20 or px[row + c*3 + 1] > 20 or px[row + c*3 + 2] > 20]
    mark = '#' * min(len(cols), 50)
    print(f"{y:3d} nb={len(cols):4d} {cols[0] if cols else '-':>4}-{cols[-1] if cols else '-':<4} {mark}")
