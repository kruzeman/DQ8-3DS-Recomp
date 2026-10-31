#!/usr/bin/env python3
"""Find nonzero 16-bit words and their offsets in a DSP dump (coarse map)."""
import struct
import sys

path = sys.argv[1]
with open(path, 'rb') as f:
    blob = f.read()
print('%s: %d bytes' % (path, len(blob)))
runs = []
in_run = False
start = 0
for off in range(0, len(blob) - 1, 2):
    v = struct.unpack_from('<H', blob, off)[0]
    if v != 0:
        if not in_run:
            in_run = True
            start = off
    else:
        if in_run:
            # only end the run after 64 consecutive zero words (256 B)
            nz = False
            for k in range(off, min(off + 256, len(blob) - 1), 2):
                if struct.unpack_from('<H', blob, k)[0] != 0:
                    nz = True
                    break
            if not nz:
                runs.append((start, off))
                in_run = False
if in_run:
    runs.append((start, len(blob)))
for a, b in runs:
    print('nonzero %06X..%06X (%d bytes)' % (a, b, b - a))
