#!/usr/bin/env python3
"""t_41f38df0: locate float words inside a captured PICA command list.

usage: cmdlist_find_float.py <cmdbin> <list_addr_hex> <f32> [<f32> ...]

Prints every offset in the list where any of the given float32 bit patterns
occurs (byte-aligned and at +1/+2/+3 word-ish alignment), so the guest VA
range can be handed to DQ8_TRACE_WRITE=<addr>:<len> to catch the CPU code
that uploads the cutscene camera.
"""
import struct
import sys

path, addr = sys.argv[1], int(sys.argv[2], 16)
vals = [float(x) for x in sys.argv[3:]]
data = open(path, "rb").read()
print(f"{path}: {len(data)} bytes at guest {addr:#010x}")

patterns = [(v, struct.pack("<f", v)) for v in vals]
hits = {}
for v, pat in patterns:
    start = 0
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        hits.setdefault(i, []).append(v)
        start = i + 1

# also report float24-ish (low 3 bytes of the f32) matches at +-1/2/3 offsets
for i in sorted(hits):
    print(f"  +{i:#06x} (va {addr + i:#010x}) words: "
          + ", ".join(f"{v!r}" for v in hits[i]))

# dump a window of words around every hit cluster
if hits:
    lo = min(hits) & ~3
    hi = (max(hits) + 4) & ~3
    print(f"--- words {lo:#x}..{hi:#x} ---")
    for off in range(lo, min(hi, len(data)), 4):
        w = struct.unpack_from("<I", data, off)[0]
        f = struct.unpack_from("<f", data, off)[0]
        mark = " <<<" if off in hits else ""
        print(f"  {off:#06x} {w:08X} {f!r}{mark}")
else:
    print("  no f32 match; dumping first 64 words for inspection")
    for off in range(0, min(256, len(data)), 4):
        w = struct.unpack_from("<I", data, off)[0]
        f = struct.unpack_from("<f", data, off)[0]
        print(f"  {off:#06x} {w:08X} {f!r}")
