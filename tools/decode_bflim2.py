#!/usr/bin/env python3
"""General BFLIM footer decoder (sqex field layout, no logo assumptions)."""
import sys, struct
from pathlib import Path
for p in sys.argv[1:]:
    data = Path(p).read_bytes()
    footer = len(data) - 40
    if footer < 0 or data[footer:footer+4] != b'FLIM':
        print(f'{p}: no FLIM footer (len={len(data)})'); continue
    bom = struct.unpack_from('<H', data, footer+4)[0]
    sub = struct.unpack_from('<H', data, footer+6)[0]
    version = struct.unpack_from('<I', data, footer+8)[0]
    fsize = struct.unpack_from('<I', data, footer+12)[0]
    raw = struct.unpack_from('<I', data, footer+36)[0]
    w, h, align, fmt, orient = struct.unpack_from('<HHHBB', data, footer+28)
    print(f'{p} (len={len(data)}): ver=0x{version:08x} bom=0x{bom:04x} fsize={fsize} '
          f'w={w} h={h} align={align} fmt={fmt} orient={orient} raw={raw}')
