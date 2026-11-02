#!/usr/bin/env python3
"""Analyze a PICA vertex-shader program dump (DQ8_VSPROGDUMP=1).

Decodes the microcode the way src/pica_vs.c (pica_execute_vs / pica_vs_source)
does and reports the instruction mix, the operand kinds and the swizzle shapes,
so an interpreter fast path can be aimed at what actually executes.

usage: tools/vsprog_analyze.py progdump.txt
"""
import collections
import re
import sys

SHIFTS = (11, 20, 29)
SIGNS = (4, 13, 22)


def src_desc(s, n, rel, sw, operand):
    """Mirror pica_vs_source: return (kind, index, perm, negs)."""
    sh = SHIFTS[operand]
    perm = tuple((sw >> (sh - 2 * k)) & 3 for k in range(4))
    neg = bool((sw >> SIGNS[operand]) & 1)
    if n < 16:
        return ("input", n, perm, neg)
    if n < 32:
        return ("temp", n - 16, perm, neg)
    i = n - 32
    if rel:
        return ("uniform-rel", i, perm, neg)
    return ("uniform" if i < 96 else "const1", i, perm, neg)


def main(path):
    prog, swz = {}, {}
    for line in open(path, errors="replace"):
        m = re.match(r"VSPROG (\d+) ([0-9A-Fa-f]{8})", line)
        if m:
            prog[int(m.group(1))] = int(m.group(2), 16)
            continue
        m = re.match(r"VSSWZ (\d+) ([0-9A-Fa-f]{8})", line)
        if m:
            swz[int(m.group(1))] = int(m.group(2), 16)
    print(f"program words: {len(prog)}   swizzle descs: {len(swz)}")
    ops = collections.Counter()
    arith_ops = collections.Counter()
    kinds = collections.Counter()
    perm_shape = collections.Counter()
    rel_ops = 0
    masked = collections.Counter()
    for pc in sorted(prog):
        w = prog[pc]
        op = w >> 26
        ops[op] += 1
        if op in (0x21, 0x22) or 0x24 <= op <= 0x28 or op in (0x2C, 0x2D):
            continue
        mad = op >= 0x30
        inv = (0x18 <= op <= 0x1B) or (0x30 <= op < 0x38)
        desc = w & (31 if mad else 127)
        sw = swz.get(desc, 0)
        mask = sw & 15
        masked[mask] += 1
        if mad:
            dur = w >> 24
            a = src_desc(None, (w >> 17) & 31, 0, sw, 0)
            b = src_desc(None, (w >> 12) & 31 if inv else (w >> 10) & 127,
                         0 if inv else ((w >> 22) & 3), sw, 1)
            c = src_desc(None, (w >> 5) & 127 if inv else (w >> 5) & 31,
                         ((w >> 22) & 3) if inv else 0, sw, 2)
            cands = [a, b, c]
        else:
            dur = w >> 21
            a = src_desc(None, (w >> 14) & 31 if inv else (w >> 12) & 127,
                         0 if inv else ((w >> 19) & 3), sw, 0)
            b = src_desc(None, (w >> 7) & 127 if inv else (w >> 7) & 31,
                         ((w >> 19) & 3) if inv else 0, sw, 1)
            cands = [a, b]
        arith_ops[op] += 1
        for cand in cands:
            kinds[cand[0]] += 1
            if cand[0] == "uniform-rel":
                rel_ops += 1
            perm_shape[(cand[0], cand[2], cand[3])] += 1
    print("\nop histogram (op: count):")
    for op, c in sorted(ops.items(), key=lambda kv: -kv[1]):
        print(f"  0x{op:02X}  {c}")
    print("\noperand kinds:")
    for k, c in kinds.most_common():
        print(f"  {k:12s} {c}")
    print(f"\nrelative-addressed operands: {rel_ops}")
    print("\nwrite masks (sw&15):")
    for k, c in masked.most_common():
        print(f"  mask {k:x}: {c}")
    print("\ntop operand shapes (kind, perm, neg):")
    for k, c in perm_shape.most_common(12):
        print(f"  {k[0]:12s} perm={k[1]} neg={int(k[2])}: {c}")
    ident = sum(1 for k in perm_shape if k[1] == (0, 1, 2, 3) and not k[2])
    tot = sum(perm_shape.values())
    print(f"\nidentity swizzle: {ident}/{tot} = {100.0*ident/tot:.1f}% of operands")


if __name__ == "__main__":
    main(sys.argv[1])
