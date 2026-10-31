#!/usr/bin/env python3
"""Scan DQ8.code for 32-bit LE pointers to a guest address (xref hunt).
usage: tools/find_ptr_xrefs.py 0xADDR [max_hits]
"""
import struct
import sys

CODE = "DQ8.code"
TEXT = 0x00100000


def main():
    target = int(sys.argv[1], 0) & 0xFFFFFFFF
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    needle = struct.pack("<I", target)
    with open(CODE, "rb") as f:
        data = f.read()
    hits = []
    start = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        hits.append(TEXT + i)
        start = i + 1
        if len(hits) >= limit:
            break
    print("xrefs to 0x%08X: %d%s" % (target, len(hits),
          "" if len(hits) < limit else "+ (capped)"))
    for h in hits:
        print("  0x%08X" % h)


if __name__ == "__main__":
    main()
