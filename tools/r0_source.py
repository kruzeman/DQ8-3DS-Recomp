#!/usr/bin/env python3
# Walk backwards from a BL call site and report every instruction that writes
# r0, resolving BL targets and pc-relative literals. Answers "where did the
# first argument come from?".
# Usage: python3 tools/r0_source.py 0x003B178C [back=48]
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


def bl_target(addr, w):
    if (w & 0x0E000000) == 0x0A000000:
        imm24 = w & 0x00FFFFFF
        if imm24 & 0x800000:
            imm24 -= 0x1000000
        return addr + 8 + (imm24 << 2)
    return None


def describe(addr, w):
    if w is None:
        return "<none>"
    if (w & 0x0FFFFFF0) == 0x012FFF10:
        return "bx r%d" % (w & 0xF)
    if (w & 0x0FFFFFF0) == 0x012FFF30:
        return "blx r%d" % (w & 0xF)
    if (w & 0x0F000000) == 0x0B000000:
        return "BL -> 0x%08X" % bl_target(addr, w)
    if (w & 0x0F000000) == 0x0A000000:
        return "B -> 0x%08X" % bl_target(addr, w)
    # data processing immediate, test r0 dest
    if (w & 0x0FE00000) == 0x02400000 or (w & 0x0FE00000) == 0x02800000:
        op = "sub" if (w & 0x01E00000) == 0x00400000 else "add"
        if ((w >> 16) & 0xF) == 0:
            rot = ((w >> 8) & 0xF) * 2
            imm8 = w & 0xFF
            imm = ((imm8 >> rot) | (imm8 << (32 - rot))) & 0xFFFFFFFF if rot else imm8
            rn = (w >> 16) & 0xF
            return "%s r0, r%d, #0x%X" % (op, rn, imm)
    # mov immediate
    if (w & 0x0FE00000) == 0x03A00000 and ((w >> 12) & 0xF) == 0:
        rot = ((w >> 8) & 0xF) * 2
        imm8 = w & 0xFF
        imm = ((imm8 >> rot) | (imm8 << (32 - rot))) & 0xFFFFFFFF if rot else imm8
        return "mov r0, #0x%X" % imm
    # mov reg
    if (w & 0x0FFF0FF0) == 0x01A00000 and ((w >> 12) & 0xF) == 0:
        return "mov r0, r%d" % (w & 0xF)
    # ldr/str immediate, pre-index, dest/源 r0
    if (w & 0x0E500000) == 0x04100000 or (w & 0x0E500000) == 0x05100000:
        L = (w >> 20) & 1
        rn = (w >> 16) & 0xF
        rd = (w >> 12) & 0xF
        off = w & 0xFFF
        if L and rd == 0:
            return "ldr r0, [r%d, #0x%X]" % (rn, off)
        if not L and rn == 0:
            return "str r%d, [r0, #0x%X]" % (rd, off)
    # pc-relative ldr -> literal
    if (w & 0x0E5F0000) == 0x051F0000 and ((w >> 12) & 0xF) == 0:
        add = (w >> 23) & 1
        off = w & 0xFFF
        lit_addr = (addr + 8 + off) if add else (addr + 8 - off)
        lit = word_at(lit_addr)
        return "ldr r0, [pc, #0x%X] -> lit@0x%08X = %s" % (
            off, lit_addr, ("0x%08X" % lit) if lit is not None else "?")
    # ldr r0, [rX] simple
    if (w & 0x0E500FFF) == 0x05100000 and ((w >> 12) & 0xF) == 0 and ((w >> 16) & 0xF) != 0xF:
        return "ldr r0, [r%d]" % ((w >> 16) & 0xF)
    return ""


site = int(sys.argv[1], 16)
back = int(sys.argv[2]) if len(sys.argv) > 2 else 48
print("call site 0x%08X:" % site)
for a in range(site, site - back * 4, -4):
    w = word_at(a)
    d = describe(a, w)
    if d:
        print("  0x%08X: %08X  %s" % (a, w, d))
