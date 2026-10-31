#!/usr/bin/env python3
"""t_41f38df0: find the enclosing function entry for a guest PC and every BL
site that calls it (DQ8.code is a flat image at 0x00100000).

usage: find_func_xrefs.py <pc_hex> [max_back]
"""
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000

pc = int(sys.argv[1], 16)
max_back = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x4000
blob = open(CODE, "rb").read()


def w_at(addr):
    off = addr - BASE
    if off < 0 or off + 4 > len(blob):
        return None
    return struct.unpack_from("<I", blob, off)[0]


# walk back for `stmdb sp!, {.., lr}` = 0xE92D.... with LR (bit 14) set
entry = None
for addr in range(pc & ~3, (pc & ~3) - max_back, -4):
    w = w_at(addr)
    if w is None:
        break
    if (w & 0xFFFF0000) == 0xE92D0000 and (w & 0x4000):
        entry = addr
        break
print(f"pc={pc:#010x} -> function entry {entry and hex(entry)}")

if not entry:
    sys.exit(1)

# every BL/BLX to that entry: 0xEB...... (BL) encoding
hits = []
for off in range(0, len(blob) - 4, 4):
    w = struct.unpack_from("<I", blob, off)[0]
    if (w & 0x0F000000) != 0x0B000000:   # BL (cond any) 0x*B*
        continue
    imm = w & 0x00FFFFFF
    if imm & 0x00800000:
        imm -= 0x01000000
    src = BASE + off
    if src + 8 + imm * 4 == entry:
        hits.append(src)
print(f"BL xrefs to {entry:#010x}: {len(hits)}")
for h in hits[:40]:
    print(f"  {h:#010x}")
