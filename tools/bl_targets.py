#!/usr/bin/env python3
# Disassemble a guest range via llvm, and resolve BL/B/BX targets arithmetically
# from the raw instruction words (llvm prints section-relative numbers that are
# easy to misread).
# Usage: python3 tools/bl_targets.py 0x001360E4 [count] [thumb]
import re
import struct
import subprocess
import sys
import tempfile

START = int(sys.argv[1], 16)
COUNT = int(sys.argv[2]) if len(sys.argv) > 2 else 64
THUMB = len(sys.argv) > 3 and sys.argv[3] == "thumb"
STEP = 2 if THUMB else 4
CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000

with open(CODE, "rb") as fh:
    fh.seek(START - BASE)
    data = fh.read(COUNT * STEP)


def target(addr, w):
    """Return absolute guest target for a conditional/unconditional branch."""
    if THUMB:
        # Thumb-2 B/BL with 32-bit form or 16-bit
        return None
    if (w & 0x0E000000) == 0x0A000000:  # B / BL
        imm24 = w & 0x00FFFFFF
        if imm24 & 0x800000:
            imm24 -= 0x1000000
        return addr + 8 + (imm24 << 2)
    return None


words = [struct.unpack_from("<I", data, i)[0] for i in range(0, len(data), 4)]

# ask llvm for text
with tempfile.NamedTemporaryFile("w", suffix=".s", delete=False) as fh:
    fh.write(".syntax unified\n.arch armv7-a\n.text\n.align 2\n")
    for i in range(0, len(words), 8):
        fh.write(".word %s\n" % ", ".join("0x%08X" % w for w in words[i:i + 8]))
    src = fh.name
obj = src + ".o"
subprocess.run(["llvm-mc", "-filetype=obj", "--triple=armv7-none-eabi", src,
                "-o", obj], check=True)
out = subprocess.run(["llvm-objdump", "-d", "--no-show-raw-insn",
                      "--triple=armv7", obj],
                     capture_output=True, text=True).stdout

pat = re.compile(r"^\s*([0-9a-f]+):\s+(.*)$")
i = 0
for line in out.splitlines():
    m = pat.match(line)
    if not m:
        continue
    off = int(m.group(1), 16)
    text = m.group(2).strip()
    addr = START + off
    w = words[off // 4] if off // 4 < len(words) else None
    note = ""
    if w is not None:
        t = target(addr, w)
        if t is not None:
            note = "  -> 0x%08X%s" % (t, " (BL)" if (w & 0x01000000) else "")
        elif (w & 0x0FFFFFF0) == 0x012FFF10:  # bx
            note = "  -> bx r%d" % (w & 0xF)
        elif (w & 0x0FFFFFF0) == 0x012FFF30:  # blx reg
            note = "  -> blx r%d" % (w & 0xF)
    print("0x%08X: %-46s%s" % (addr, text, note))
