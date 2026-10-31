#!/usr/bin/env python3
# Find every occurrence of given 32-bit words in DQ8.code and print context.
# Usage: python3 tools/find_word.py 0x131E [more_words...]
import struct, sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000
words = [int(w, 16) for w in sys.argv[1:]]
with open(CODE, "rb") as fh:
    data = fh.read()
for w in words:
    pat = struct.pack("<I", w)
    start = 0
    print("--- word 0x%08X ---" % w)
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        print("  @0x%08X (raw off 0x%X)" % (BASE + i, i))
        start = i + 1
