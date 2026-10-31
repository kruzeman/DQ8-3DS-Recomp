#!/usr/bin/env python3
"""Find pointer tables (vtables) in DQ8.code that reference a given address."""
import struct
import sys

target = int(sys.argv[1], 16)
needle = struct.pack('<I', target)
with open('/home/hermes/DQ8/DQ8.code', 'rb') as fh:
    data = fh.read()
pos = 0
while True:
    pos = data.find(needle, pos)
    if pos < 0:
        break
    if pos % 4 == 0:
        base = pos
        # walk outward while entries look like code pointers (0x00100000..0x00540000, aligned)
        def ok(off):
            v = struct.unpack_from('<I', data, off)[0]
            return 0x00100000 <= v < 0x0053BCB4 and v % 4 == 0
        start = base
        while ok(start - 4):
            start -= 4
        end = base
        while ok(end + 4):
            end += 4
        entries = [struct.unpack_from('<I', data, o)[0] for o in range(start, end + 4, 4)]
        print('table @0x%08X..0x%08X (%d entries):' %
              (0x00100000 + start, 0x00100000 + end, len(entries)))
        print(' '.join('%08X' % v for v in entries))
    pos += 4
