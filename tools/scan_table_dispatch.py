#!/usr/bin/env python3
"""Extract table-driven `add pc, rY, rX` dispatch targets from guest code.

DQ8's movie decoder also dispatches with a word-offset table:

    sub   r6, pc, #imm          ; r6 = table base   (pc-relative)
    ldr   r5, [r6, r5, lsl #2]  ; r5 = table[index]
    ...
    add   pc, r5, r6            ; target = base + table[index]

The table is an array of little-endian 32-bit offsets from the base. This tool
finds every `add pc, rm, rn` site in a bounded region, recovers the pc-relative
table base, reads candidate entries, and keeps only entries whose target looks
like a real handler entry. Accepted prologues: `stmdb sp!, {...}` (push of a
register list), `str rd, [sp, #-4]!` (single-register push, the usual encoding
of `push {lr}`), optionally preceded by up to two `mov rd, #imm` register-setup
instructions (small thunks that set arguments before falling into a handler).

usage: tools/scan_table_dispatch.py <code-file> <load-base> <start> <end> \
           [existing-roots-file] [max-table-words]
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


def is_sub_imm_pc(w, rd):
    """sub rd, pc, #imm  (immediate, S=0)."""
    return ((w >> 25) & 1) == 1 and ((w >> 21) & 0xF) == 0b0010 \
        and ((w >> 20) & 1) == 0 and ((w >> 16) & 0xF) == 0xF \
        and ((w >> 12) & 0xF) == rd


def is_add_reg_pc(w, rd):
    """add pc, rn, rm (register form, shift #0, S=0)."""
    return ((w >> 25) & 7) == 0b000 and ((w >> 21) & 0xF) == 0b0100 \
        and ((w >> 20) & 1) == 0 and ((w >> 12) & 0xF) == rd \
        and (w & 0x0FF0) == 0 and ((w >> 4) & 1) == 0


def is_stm_push(w):
    """stmdb sp!, {...} — push of a register list."""
    return (w & 0x0FFF0000) == 0x092D0000


def is_str_push(w):
    """str rd, [sp, #-4]! — single-register push (assembler form of
    `push {lr}`): STR immediate, P=1 U=0 B=0 W=1 L=0, Rn=sp, imm12=4."""
    return (w & 0x0FFF0FFF) == 0x052D0004


def is_mov_imm(w):
    """mov rd, #imm (immediate form, S=0, Rn=0)."""
    return ((w >> 25) & 7) == 0b001 and ((w >> 21) & 0xF) == 0b1101 \
        and ((w >> 20) & 1) == 0 and ((w >> 16) & 0xF) == 0


def is_handler_entry(data, toff):
    """Target starts a handler: a push form within the first three words,
    allowing up to two leading `mov rd, #imm` register-setup instructions
    (small argument-setting thunks)."""
    if toff < 0 or toff + 12 > len(data):
        return False
    for i in range(3):
        w = u32(data, toff + 4 * i)
        if is_stm_push(w) or is_str_push(w):
            return True
        if not is_mov_imm(w):
            return False
    return False


def main():
    code_path = sys.argv[1]
    base = int(sys.argv[2], 0)
    start = int(sys.argv[3], 0)
    end = int(sys.argv[4], 0)
    roots_path = sys.argv[5] if len(sys.argv) > 5 else None
    max_words = int(sys.argv[6]) if len(sys.argv) > 6 else 40

    data = open(code_path, "rb").read()
    roots = set()
    if roots_path:
        for line in open(roots_path):
            line = line.strip()
            if line.startswith("0x"):
                roots.add(int(line, 16))

    off0 = start - base
    off1 = min(end - base, len(data) - 4)
    seen = set()
    for off in range(off0, off1, 4):
        w = u32(data, off)
        if not is_add_reg_pc(w, 15):
            continue
        rn = (w >> 16) & 0xF
        rm = w & 0xF
        if rn == 0xF or rm == 0xF:
            continue
        # walk back up to 16 instructions for: sub <rN or rM>, pc, #imm
        base_addr = None
        site = base + off
        for back in range(4, 68, 4):
            p = off - back
            if p < off0:
                break
            wb = u32(data, p)
            for cand in (rn, rm):
                if is_sub_imm_pc(wb, cand):
                    pc_here = base + p + 8
                    base_addr = (pc_here - expand_imm(wb & 0xFFF)) & 0xFFFFFFFF
                    break
            if base_addr is not None:
                break
        if base_addr is None:
            print(f"dispatch@{site:08X} add pc,r{rn},r{rm}  (no pc-relative base found)")
            continue
        # read candidate table entries
        good = []
        bad_streak = 0
        for i in range(max_words):
            toff = base_addr + 4 * i - base
            if toff < 0 or toff + 4 > len(data):
                break
            val = u32(data, toff)
            target = (base_addr + val) & 0xFFFFFFFF
            ok = (val & 3) == 0 and 0 < val < 0x20000 \
                and start <= target < end
            if ok:
                toff_t = target - base
                ok = is_handler_entry(data, toff_t)
            if ok:
                good.append(target)
                bad_streak = 0
            else:
                bad_streak += 1
                if bad_streak >= 4 and good:
                    break
        if not good:
            print(f"dispatch@{site:08X} table@{base_addr:08X}  "
                  f"(base recovered, but no valid handler entries)")
            continue
        if (site, base_addr) in seen:
            continue
        seen.add((site, base_addr))
        missing = [t for t in good if t not in roots]
        print(f"dispatch@{site:08X} table@{base_addr:08X} entries={len(good)} "
              f"missing={len(missing)}")
        for t in good:
            mark = "MISSING" if t not in roots else "   root"
            print(f"    {mark} 0x{t:08X}")


if __name__ == "__main__":
    main()
