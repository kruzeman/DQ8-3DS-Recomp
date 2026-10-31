#!/usr/bin/env python3
# check_entries.py FILE — for each hex address in FILE, report whether a
# translated case exists and the first ARM word (entry-prologue sanity).
import glob
import re
import struct
import sys

BASE = 0x00100000
cases = set()
for p in glob.glob("/home/hermes/DQ8/target/dq8-split/dq8_part_*.c"):
    with open(p, errors="ignore") as fh:
        for line in fh:
            if line.startswith("case 0x"):
                m = re.match(r"case (0x[0-9A-F]+)", line)
                if m:
                    cases.add(int(m.group(1), 16))

with open("/home/hermes/DQ8/DQ8.code", "rb") as f:
    code = f.read()


def classify(w):
    top = w & 0xFFFF0000
    if top == 0xE92D0000:
        return "push{...}"
    if (w & 0xFFF00000) == 0xE24DD000:
        return "sub-sp"
    if (w & 0xFFFF0000) == 0xE52DE000:
        return "str-lr"
    if (w & 0xFFF00000) == 0xE5900000:
        return "ldr-rd,[rn]"
    if (w & 0xFFFF0000) == 0xE59F0000:
        return "ldr-rd,[pc]"
    if w == 0xE12FFF1E:
        return "bx-lr"
    if (w & 0xFFF00000) == 0xE3A00000:
        return "mov-rd,#imm"
    if (w & 0xFFF00000) == 0xE3500000:
        return "cmp-stub"
    if (w & 0x0F000000) == 0x0A000000:
        return "branch"
    return "OTHER 0x%08X" % w


out = []
for line in open(sys.argv[1]):
    line = line.split()[0] if line.split() else ""
    if not line:
        continue
    a = int(line, 16)
    off = a - BASE
    if off < 0 or off + 4 > len(code):
        print("0x%08X OUT-OF-RANGE" % a)
        continue
    fw = struct.unpack_from("<I", code, off)[0]
    cov = a in cases or (a & ~1) in cases
    print("0x%08X %s first=%-8s %s" % (
        a, "COVERED " if cov else "MISSING", "", classify(fw)))
    if not cov:
        out.append(a)

print()
print("missing (%d):" % len(out), " ".join("0x%08X" % x for x in out))
