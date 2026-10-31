#!/usr/bin/env python3
# bucket_missing.py — classify untranslated literal-callback candidates by
# their first ARM word (entry-prologue strength) for batch root verification.
import struct

BASE = 0x00100000
with open("/home/hermes/DQ8/DQ8.code", "rb") as f:
    code = f.read()

addrs = [int(l, 16) for l in open("/tmp/lit_missing.txt") if l.strip()]

buckets = {}
detail = []
for a in addrs:
    off = a - BASE
    if off < 0 or off + 4 > len(code):
        continue
    w = struct.unpack_from("<I", code, off)[0]
    top = w & 0xFFFF0000
    if top == 0xE92D0000 and (w & 0x4000):
        b = "push{...,lr}-entry"
    elif top == 0xE92D0000:
        b = "push-no-lr?"
    elif (w & 0xFFF00000) == 0xE24DD000:
        b = "sub-sp,sp,#imm"
    elif (w & 0xFFFF0000) == 0xE52DE000:
        b = "str-lr,[sp,#-]!"
    elif (w & 0xFFF00000) == 0xE3A00000:
        b = "mov-rd,#imm"
    elif (w & 0xFFF00000) == 0xE3500000:
        b = "cmp-rd,#imm (stub)"
    elif (w & 0xFFFF0000) == 0xE59F0000:
        b = "ldr-rd,[pc,#+]"
    elif w == 0xE12FFF1E:
        b = "bx-lr only"
    elif (w & 0xFFC00000) == 0xE1A00000:
        b = "mov-reg-form"
    else:
        b = "other/weak"
    buckets.setdefault(b, []).append((a, w))
    detail.append((b, a, w))

print("total:", len(addrs))
for b in sorted(buckets, key=lambda x: -len(buckets[x])):
    print("  %-22s %d" % (b, len(buckets[b])))
print()
for b, a, w in detail:
    if b in ("push{...,lr}-entry", "sub-sp,sp,#imm", "str-lr,[sp,#-]!", "other/weak"):
        print("  %-22s 0x%08X  first=0x%08X" % (b, a, w))

# write per-bucket address files for batch-root assembly
import collections
byb = collections.defaultdict(list)
for b, a, w in detail:
    byb[b].append(a)
for b, lst in byb.items():
    name = b.replace(",", "").replace("{", "").replace("}", "").replace("-", "_").replace("/", "_").replace(" ", "")
    with open("/tmp/bucket_%s.txt" % name, "w") as fh:
        for a in lst:
            fh.write("0x%08X\n" % a)
    print("wrote /tmp/bucket_%s.txt (%d)" % (name, len(lst)))
