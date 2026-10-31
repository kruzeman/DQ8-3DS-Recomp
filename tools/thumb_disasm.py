#!/usr/bin/env python3
# thumb_disasm.py GUEST_ADDR NBYTES — thumb disasm of DQ8.code range.
# Pipeline: bytes -> .s (.byte list, .thumb) -> llvm-mc (obj) -> llvm-objdump -d.
import re
import subprocess
import sys
import tempfile

start = int(sys.argv[1], 16)
nbytes = int(sys.argv[2])
with open("/home/hermes/DQ8/DQ8.code", "rb") as fh:
    fh.seek(start - 0x00100000)
    data = fh.read(nbytes)

with tempfile.NamedTemporaryFile("w", suffix=".s", delete=False) as fh:
    fh.write(".syntax unified\n.arch armv7-a\n.thumb\n.text\n.align 2\n")
    for i in range(0, len(data), 16):
        chunk = ", ".join("0x%02X" % b for b in data[i:i + 16])
        fh.write(".byte %s\n" % chunk)
    src = fh.name
obj = src + ".o"
subprocess.run(
    ["llvm-mc", "-filetype=obj", "--triple=thumbv7-none-eabi", src, "-o", obj],
    check=True,
)
out = subprocess.run(
    ["llvm-objdump-18", "-d", "--no-show-raw-insn", "--triple=thumbv7", obj],
    capture_output=True, text=True, check=True,
).stdout
for line in out.splitlines():
    m = re.match(r"\s*([0-9a-f]+):\s+(.*)", line)
    if m:
        print("0x%08X: %s" % (start + int(m.group(1), 16), m.group(2)))
