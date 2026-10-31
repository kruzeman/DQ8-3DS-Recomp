#!/usr/bin/env python3
# Print guest addresses of extend-family instructions matching a given op1.
# Usage: scan_extend_addrs.py OP1_HEX [START END]
import sys

with open("/home/hermes/DQ8/DQ8.code", "rb") as fh:
    blob = fh.read()

op1 = int(sys.argv[1], 16)
lo = int(sys.argv[2], 16) if len(sys.argv) > 3 else 0x00100000
hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x00100000 + len(blob)

addrs = []
for off in range(0, len(blob) - 3, 4):
    addr = 0x00100000 + off
    if not (lo <= addr < hi):
        continue
    w = int.from_bytes(blob[off:off + 4], "little")
    if (w & 0x0F0003F0) != 0x06000070:
        continue
    if ((w >> 20) & 15) != op1:
        continue
    rn = (w >> 16) & 15
    rd = (w >> 12) & 15
    rm = w & 15
    rot = ((w >> 10) & 3) * 8
    addrs.append((addr, rn, rd, rm, rot, w))

for addr, rn, rd, rm, rot, w in addrs:
    acc = "" if rn == 15 else f" rn=r{rn}"
    print(f"0x{addr:08X}: r{rd} <- ext(r{rm} ror {rot}){acc} (0x{w:08X})")
print(f"total {len(addrs)}")
