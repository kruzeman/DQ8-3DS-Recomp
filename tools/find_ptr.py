#!/usr/bin/env python3
# find_ptr.py VALUE [VALUE...] — scan DQ8.code for words equal to VALUE and
# print guest addresses + 6-word neighbourhood, to locate pointer tables.
import sys

vals = [int(v, 16) for v in sys.argv[1:]]
with open("/home/hermes/DQ8/DQ8.code", "rb") as f:
    data = f.read()

for v in vals:
    needle = v.to_bytes(4, "little")
    hits = []
    i = 0
    while True:
        i = data.find(needle, i)
        if i < 0:
            break
        if i % 4 == 0:
            hits.append(i)
        i += 4
    print("== value 0x%08X: %d word hits ==" % (v, len(hits)))
    for off in hits[:40]:
        ga = off + 0x00100000
        neigh = " ".join(
            "%08X" % int.from_bytes(data[off + 4 * k: off + 4 * k + 4], "little")
            for k in range(-3, 7)
            if 0 <= off + 4 * k <= len(data) - 4
        )
        print("  0x%08X: %s" % (ga, neigh))
