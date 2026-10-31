#!/usr/bin/env python3
"""Extract `mov<cond> pc, r4` dispatch targets from guest code.

DQ8's movie decoder dispatches with this idiom (see 0x001A72A0..0x001A72AC):

    add   r4, pc, #imm1
    add   r4, r4, #imm2
    cmp   ip, #N
    movls pc, r4            ; 04 f0 a0 d1

Each such block computes one constant handler address, so the target list is
statically computable and can be verified per address before adding roots.

usage: tools/scan_dispatch_targets.py <code-file> <load-base> <start> <end>
"""
import struct
import sys


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def expand_imm(imm12):
    rot = (imm12 >> 8) & 0xF
    imm = imm12 & 0xFF
    if rot == 0:
        return imm
    return ((imm >> (2 * rot)) | (imm << (32 - 2 * rot))) & 0xFFFFFFFF


def is_add_imm_reg(w, rd, rn):
    """add rd, rn, #imm (immediate form, S=0, any cond)."""
    return ((w >> 25) & 1) == 1 and ((w >> 21) & 0xF) == 0b0100 and ((w >> 20) & 1) == 0 \
        and ((w >> 12) & 0xF) == rd and ((w >> 16) & 0xF) == rn


def main():
    code_path = sys.argv[1]
    base = int(sys.argv[2], 0)
    start = int(sys.argv[3], 0)
    end = int(sys.argv[4], 0)
    data = open(code_path, "rb").read()
    off = start - base
    stop = min(end - base, len(data) - 16)
    while off + 16 <= stop:
        w0, w1, w2, w3 = (u32(data, off + 4 * i) for i in range(4))
        if (is_add_imm_reg(w0, 4, 0xF) and is_add_imm_reg(w1, 4, 4)
                and (w3 & 0x0FFFFFFF) == 0x01A0F004):
            pc0 = base + off + 8
            target = (pc0 + expand_imm(w0 & 0xFFF)) & 0xFFFFFFFF
            target = (target + expand_imm(w1 & 0xFFF)) & 0xFFFFFFFF
            cond = (w3 >> 28) & 0xF
            cmp_n = expand_imm(w2 & 0xFFF) if (w2 & 0x0FE0F000) == 0x03500000 else None
            site = base + off
            print(f"dispatch@{site:08X} cond={cond:X} cmp_ip<={cmp_n if cmp_n is None else hex(cmp_n)} -> {target:08X}")
        off += 4


if __name__ == "__main__":
    main()
