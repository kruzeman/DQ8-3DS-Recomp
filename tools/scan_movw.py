#!/usr/bin/env python3
# Find MOVW/MOVT immediate pairs or single MOVW loading a given 32-bit constant.
# Usage: python3 tools/scan_movw.py <const_hex> [scan_lo_hex] [scan_hi_hex]
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000

const = int(sys.argv[1], 16)
slo = int(sys.argv[2], 16) if len(sys.argv) > 2 else BASE
shi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x00600000

with open(CODE, "rb") as fh:
    data = fh.read()

def movw_imm(w):
    # MOVW: cond 0011 0000 imm4 Rd imm12  -> bits[27:20] = 00110000
    if (w & 0x0FF00000) == 0x03000000:
        imm4 = (w >> 16) & 0xF
        imm3 = (w >> 12) & 0x7
        imm8 = w & 0xFF
        return (imm4 << 12) | (imm3 << 8) | imm8, (w >> 12) & 0xF
    return None

def movt_imm(w):
    # MOVT: cond 0011 0100 imm4 Rd imm12 -> bits[27:20] = 00110100
    if (w & 0x0FF00000) == 0x03400000:
        imm4 = (w >> 16) & 0xF
        imm3 = (w >> 12) & 0x7
        imm8 = w & 0xFF
        return (imm4 << 12) | (imm3 << 8) | imm8, (w >> 12) & 0xF
    return None

hits = []
for off in range(0, len(data) - 3, 4):
    addr = BASE + off
    if addr < slo or addr >= shi:
        continue
    w = struct.unpack_from("<I", data, off)[0]
    m = movw_imm(w)
    if m and m[0] == (const & 0xFFFF):
        # check for MOVT following within 8 instrs with matching high half
        hi = (const >> 16) & 0xFFFF
        found_movt = False
        for k in range(1, 12):
            if off + 4 * k + 4 > len(data):
                break
            w2 = struct.unpack_from("<I", data, off + 4 * k)[0]
            mt = movt_imm(w2)
            if mt and mt[1] == m[1] and mt[0] == hi:
                hits.append((addr, addr + 4 * k, m[1]))
                found_movt = True
                break
        if not found_movt:
            hits.append((addr, None, m[1]))

print(f"{len(hits)} MOVW site(s) loading {const:#010x}")
for a1, a2, rd in hits:
    tail = f" + MOVT @ {a2:#010x}" if a2 else " (no matching MOVT nearby)"
    print(f"  {a1:#010x}: MOVW r{rd}, #{const & 0xFFFF:#06x}{tail}")
