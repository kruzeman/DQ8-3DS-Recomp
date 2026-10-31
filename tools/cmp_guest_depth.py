#!/usr/bin/env python3
"""Compare two guest depth dumps (DQ8_DUMP_GUEST_DIR/guest_depth.raw) row by
row: where do they differ, where are they zero.

Usage: cmp_guest_depth.py <dirA> <dirB>
"""
import os
import struct
import sys

XLUT = (0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15)
YLUT = (0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a)


def load(d):
    meta = open(os.path.join(d, "guest_meta.txt")).read().split()
    rt_w, rt_h = int(meta[1]), int(meta[2])
    depth_bpp = int(meta[meta.index("depth_bpp") + 1])
    raw = open(os.path.join(d, "guest_depth.raw"), "rb").read()
    return rt_w, rt_h, depth_bpp, raw


def z(raw, rt_w, depth_bpp, row, col):
    off = ((row >> 3) * (rt_w >> 3) + (col >> 3)) * 64 + XLUT[col & 7] + YLUT[row & 7]
    i = off * depth_bpp
    if i + depth_bpp > len(raw):
        return None
    if depth_bpp == 2:
        return struct.unpack_from("<H", raw, i)[0]
    return struct.unpack_from("<I", raw, i)[0] & 0xFFFFFF


def main():
    wa, ha, ba, ra = load(sys.argv[1])
    wb, hb, bb, rb = load(sys.argv[2])
    print(f"A: {wa}x{ha} bpp={ba} bytes={len(ra)}")
    print(f"B: {wb}x{hb} bpp={bb} bytes={len(rb)}")
    print("row | A: zeros/total, distinct | B: zeros/total, distinct | diff px")
    for row in range(0, 80, 4):
        za = [z(ra, wa, ba, row, c) for c in range(wa)]
        zb = [z(rb, wb, bb, row, c) for c in range(wb)]
        za = [v for v in za if v is not None]
        zb = [v for v in zb if v is not None]
        da = sorted(set(za))
        db = sorted(set(zb))
        diff = sum(1 for x, y in zip(za, zb) if x != y)
        print(f" {row:3d} | A zeros={za.count(0):4d}/{len(za):4d} vals={da[:4]} | "
              f"B zeros={zb.count(0):4d}/{len(zb):4d} vals={db[:4]} | diff={diff}")
    # whole-buffer comparison
    if ba == bb and wa == wb and ha == hb:
        diff = zeros_a = zeros_b = 0
        n = wa * ha
        for row in range(ha):
            for col in range(wa):
                x = z(ra, wa, ba, row, col)
                y = z(rb, wb, bb, row, col)
                if x is None or y is None:
                    continue
                zeros_a += (x == 0)
                zeros_b += (y == 0)
                diff += (x != y)
        print(f"whole buffer: {n} px, A zeros={zeros_a} ({zeros_a * 100.0 / n:.1f}%), "
              f"B zeros={zeros_b} ({zeros_b * 100.0 / n:.1f}%), differing={diff} "
              f"({diff * 100.0 / n:.1f}%)")


main()
