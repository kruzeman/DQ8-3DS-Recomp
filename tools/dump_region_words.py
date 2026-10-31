#!/usr/bin/env python3
"""Dump raw A32 words of the movie region for decoder reference."""
import sys

base = 0x00100000
start = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x001A93A8
end = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x001A9410

with open("DQ8.code", "rb") as fh:
    for addr in range(start, end, 4):
        fh.seek(addr - base)
        w = int.from_bytes(fh.read(4), "little")
        print(
            f"0x{addr:08X}: 0x{w:08X}  "
            f"cond={w >> 28:01X} I={(w >> 25) & 1:X} op={(w >> 21) & 0xF:X} "
            f"S={(w >> 20) & 1:X} rn={(w >> 16) & 0xF:X} rd={(w >> 12) & 0xF:X} "
            f"rot={(w >> 8) & 0xF:X} imm={w & 0xFF:02X} rm={w & 0xF:X}"
        )
