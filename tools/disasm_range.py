#!/usr/bin/env python3
# Disassemble a guest-address range of DQ8.code.
# Pipeline: raw words -> .s (.word list) -> llvm-mc (object) -> llvm-objdump -d.
# Usage: disasm_range.py START COUNT [thumb]
import re
import subprocess
import sys
import tempfile

start = int(sys.argv[1], 16)
count = int(sys.argv[2])
thumb = len(sys.argv) > 3 and sys.argv[3] == "thumb"
with open("/home/hermes/DQ8/DQ8.code", "rb") as fh:
    fh.seek(start - 0x00100000)
    data = fh.read(count * (2 if thumb else 4))

words = [
    int.from_bytes(data[i:i + (2 if thumb else 4)], "little")
    for i in range(0, len(data), (2 if thumb else 4))
]
with tempfile.NamedTemporaryFile("w", suffix=".s", delete=False) as fh:
    fh.write(".syntax unified\n.arch armv7-a\n.text\n.align 2\n")
    for i in range(0, len(words), 8):
        chunk = ", ".join("0x%08X" % w for w in words[i:i + 8])
        fh.write(".word %s\n" % chunk)
    src = fh.name
obj = src + ".o"
subprocess.run(
    ["llvm-mc", "-filetype=obj", "--triple=armv7-none-eabi", src, "-o", obj],
    check=True,
)
out = subprocess.run(
    ["llvm-objdump-18", "-d", "--no-show-raw-insn", obj],
    capture_output=True, text=True, check=True,
).stdout
step = 2 if thumb else 4
for line in out.splitlines():
    m = re.match(r"\s*([0-9a-f]+):\s+(.*)", line)
    if m:
        print("0x%08X: %s" % (start + int(m.group(1), 16), m.group(2)))
    elif line.strip():
        print(line)
