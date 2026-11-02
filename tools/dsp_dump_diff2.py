#!/usr/bin/env python3
"""Compare two 512 KiB DSP-window dumps (DQ8_DSP_DUMP) field by field.
Usage: python3 tools/dsp_dump_diff2.py A.bin B.bin
"""
import sys

A, B = sys.argv[1], sys.argv[2]
a = open(A, 'rb').read()
b = open(B, 'rb').read()
print("sizes", len(a), len(b))

regions = (("region0", 0x50000), ("region1", 0x70000))
fields = (
    ("status", 0x820, 8),
    ("app_dsp_config", 0xE20, 16),
    ("dsp_config", 0x2860, 8),
    ("frame_counter", 0x7FFE, 2),
    ("final[0:4]", 0x5224, 16),
)
for name, base in regions:
    print("==", name)
    for fname, off, n in fields:
        va, vb = a[base + off:base + off + n], b[base + off:base + off + n]
        mark = "SAME" if va == vb else "DIFF"
        print("  %-16s %s  A=%s  B=%s" % (fname, mark, va.hex(), vb.hex()))
    for label, off0, stride, size in (("src_status", 0xA80, 12, 12),
                                      ("src_config", 0x800, 192, 24)):
        ndiff = 0
        first = None
        for i in range(24):
            off = base + off0 + i * stride
            va, vb = a[off:off + size], b[off:off + size]
            if va != vb:
                ndiff += 1
                if first is None:
                    first = (i, va.hex(), vb.hex())
        print("  %-16s %d/24 differ %s" % (label, ndiff, first or ""))
    ndiff = sum(1 for i in range(base, base + 0x8000) if a[i] != b[i])
    print("  window bytes differing: %d / %d" % (ndiff, 0x8000))
