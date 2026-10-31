#!/usr/bin/env python3
"""Quick non-black stats for PPM frames (P6)."""
import sys
import os

for path in sys.argv[1:]:
    with open(path, 'rb') as f:
        data = f.read()
    # header: P6\n<w> <h>\n<max>\n
    parts = data.split(b'\n', 3)
    if parts[0] != b'P6':
        print(f'{os.path.basename(path)}: not P6')
        continue
    w, h = map(int, parts[1].split())
    payload = parts[3]
    nz = sum(1 for b in payload if b)
    # mean brightness
    mean = sum(payload) / len(payload)
    # distinct colors approx
    print(f'{os.path.basename(path)}: {w}x{h} bytes={len(payload)} nonzero={nz} '
          f'({100.0*nz/len(payload):.2f}%) mean={mean:.1f}')
