import re, os, glob, sys
from pathlib import Path
pat = re.compile(rb'P6\s+(\d+)\s+(\d+)\s+255\s')
files = sorted(glob.glob('/tmp/dq8-frames/frame-*.ppm'),
               key=lambda f: int(re.search(r'frame-(\d+)', os.path.basename(f)).group(1)))
print('total', len(files))
best = []
for f in files:
    data = Path(f).read_bytes()
    m = pat.match(data)
    if not m:
        continue
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():]
    nb = 0
    wx = wy = 0
    for i in range(0, len(px), 3):
        if px[i] > 15 or px[i+1] > 15 or px[i+2] > 15:
            nb += 1
            x = (i // 3) % w
            y = (i // 3) // w
            if px[i] > 200 and px[i+1] > 200 and px[i+2] > 200:
                wx += 1
                wy += 1
    n = int(re.search(r'frame-(\d+)', os.path.basename(f)).group(1))
    best.append((nb, n, w, h))
# Print frames with >5 white pixels (likely the dialog), and the max content frame
best_sorted = sorted(best, key=lambda t: -t[0])
print('top 12 frames by nonblack pixel count:')
for nb, n, w, h in best_sorted[:12]:
    print(f'  frame-{n} nonblack={nb} ({w}x{h})')
# also print frames >1000 nonblack
print('frames with >1000 nonblack:')
for nb, n, w, h in sorted(best, key=lambda t: t[1]):
    if nb > 1000:
        print(f'  frame-{n} nonblack={nb}')
Path('/tmp/dq8-frame-scan.txt').write_text('\n'.join(f'{n} {nb}' for nb,n,w,h in best))
