#!/usr/bin/env python3
"""Find pc-relative LDR instructions in .text whose literal target is ADDR.
usage: tools/find_ldr_to.py 0xTARGET [0xSCAN_START 0xSCAN_END]"""
import struct
import sys

CODE = '/home/hermes/DQ8/DQ8.code'
BASE = 0x00100000
code = open(CODE, 'rb').read()

target = int(sys.argv[1], 0)
start = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x00100000
end = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x0053BCB4

hits = []
for a in range(start & ~3, end, 4):
    off = a - BASE
    if off < 0 or off + 4 > len(code):
        continue
    w = struct.unpack_from('<I', code, off)[0]
    if (w & 0x0F7F0000) != 0x051F0000:   # ldr rd, [pc, #+/-imm] (P=1,Rn=15,L=1,I=0)
        continue
    imm = w & 0xFFF
    dest = a + 8 + imm if (w >> 23) & 1 else a + 8 - imm
    if dest == target:
        rd = (w >> 12) & 0xF
        hits.append((a, rd))

print('pc-relative LDRs loading 0x%08X: %d' % (target, len(hits)))
for a, rd in hits:
    print('  0x%08X -> r%d' % (a, rd))
