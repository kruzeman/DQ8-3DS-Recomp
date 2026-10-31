#!/usr/bin/env python3
# Find callers of a guest function in DQ8.code (BL/BLX scan) and locate a
# function's prologue by scanning backwards from an address.
# Usage:
#   python3 tools/scan_calls.py callers 0x00159D3C
#   python3 tools/scan_calls.py prologue 0x00159D3C
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000

with open(CODE, "rb") as fh:
    data = fh.read()


def word_at(addr):
    i = addr - BASE
    if 0 <= i <= len(data) - 4:
        return struct.unpack_from("<I", data, i)[0]
    return None


def callers(target, branches=False):
    out = []
    for i in range(0, len(data) - 4, 4):
        w = struct.unpack_from("<I", data, i)[0]
        # plain B (tail call): cond 101 L=0 imm24
        if branches and (w & 0x0F000000) == 0x0A000000:
            imm24 = w & 0x00FFFFFF
            if imm24 & 0x800000:
                imm24 -= 0x1000000
            pc = BASE + i + 8
            if pc + (imm24 << 2) == target:
                out.append((BASE + i, "B"))
        # BL: cond 101 L=1 imm24
        if (w & 0x0F000000) == 0x0B000000:
            imm24 = w & 0x00FFFFFF
            if imm24 & 0x800000:
                imm24 -= 0x1000000
            pc = BASE + i + 8
            if pc + (imm24 << 2) == target:
                out.append((BASE + i, "BL"))
        # BLX imm: 1111 101H imm24 (always state)
        elif (w & 0xFE000000) == 0xFA000000:
            imm24 = w & 0x00FFFFFF
            if imm24 & 0x800000:
                imm24 -= 0x1000000
            pc = BASE + i + 8
            if pc + (imm24 << 2) == target:
                out.append((BASE + i, "BLX"))
    return out


def prologue(addr):
    # scan backwards for push {..., lr} (E92D xxxx with lr bit set) preceded by
    # function-align padding (0x00000000 or 0xE1A00000 nop)
    for a in range(addr, BASE, -4):
        w = word_at(a)
        if w is None:
            break
        if (w & 0xFFFF0000) == 0xE92D0000 and (w & 0x4000):  # stmfd sp!, {..., lr}
            return a
        if (w & 0xFFFF0000) == 0xE92D4000 and (w & 0x4000):
            return a
    return None


mode = sys.argv[1]
target = int(sys.argv[2], 16)
if mode in ("callers", "branches", "all"):
    res = callers(target, branches=(mode in ("branches", "all")))
    label = "entry(ies)" if mode == "callers" else "branch site(s)"
    print("%d %s of %08X" % (len(res), label, target))
    for a, kind in res:
        print("  %s %08X" % (kind, a))
elif mode == "prologue":
    p = prologue(target)
    print("prologue of %08X -> %s" % (target, ("0x%08X" % p) if p else "not found"))
