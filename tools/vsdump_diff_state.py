#!/usr/bin/env python3
"""Diff the shader state between two captured draws in a VSDUMP (t_664244f9).

Prints which VS registers, float uniforms, default attributes, program words
and swizzle words differ between draw index A and draw index B of the dump --
the fast way to see "same shader, different uniforms" vs "different program"
between the live (world) and dead (dialog) states of the character batch.

Usage: tools/vsdump_diff_state.py <dump-file> <idx-a> <idx-b>
"""
import sys
from vsdump_analyze import parse

def main():
    draws = parse(sys.argv[1])
    a, b = draws[int(sys.argv[2])], draws[int(sys.argv[3])]
    print("A: draw=%s t=%s entry=%s end=%s" % (a.meta.get("draw"), a.meta.get("t"),
                                               a.meta.get("entry"), a.end))
    print("B: draw=%s t=%s entry=%s end=%s" % (b.meta.get("draw"), b.meta.get("t"),
                                               b.meta.get("entry"), b.end))
    keys = sorted(set(a.regs) | set(b.regs))
    diffs = [r for r in keys if a.regs.get(r) != b.regs.get(r)]
    print("regs differing: %d/%d" % (len(diffs), len(keys)))
    for r in diffs:
        print("  REG %03X: %08X -> %08X" % (r, a.regs.get(r, 0), b.regs.get(r, 0)))
    n = max(len(a.uni), len(b.uni))
    ndiff = 0
    for i in range(n):
        ua = a.uni[i] if i < len(a.uni) else None
        ub = b.uni[i] if i < len(b.uni) else None
        if ua != ub:
            ndiff += 1
            print("  UNI %d: %s -> %s" % (i, ua, ub))
    print("uniforms differing: %d/%d" % (ndiff, n))
    pa, pb = a.prog, b.prog
    if pa != pb:
        print("PROGRAM differs (len %d vs %d)" % (len(pa), len(pb)))
        for i in range(max(len(pa), len(pb))):
            xa = pa[i] if i < len(pa) else None
            xb = pb[i] if i < len(pb) else None
            if xa != xb:
                print("  PROG %d: %s -> %s" % (i, xa, xb))
    else:
        print("program identical (%d words)" % len(pa))
    if a.swz != b.swz:
        print("SWIZZLE differs")
        for i in range(max(len(a.swz), len(b.swz))):
            xa = a.swz[i] if i < len(a.swz) else None
            xb = b.swz[i] if i < len(b.swz) else None
            if xa != xb:
                print("  SWZ %d: %s -> %s" % (i, xa, xb))
    else:
        print("swizzle identical (%d words)" % len(a.swz))
    if a.verts and b.verts:
        for i in range(min(len(a.verts), len(b.verts))):
            va, vb = a.verts[i], b.verts[i]
            print("vertex %d inputs differing:" % i)
            for r in range(16):
                ia, ib = va["in"].get(r), vb["in"].get(r)
                if ia != ib:
                    print("  IN %d: %s -> %s" % (r, ia, ib))

if __name__ == "__main__":
    main()
