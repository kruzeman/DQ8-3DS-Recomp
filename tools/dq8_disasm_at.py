#!/usr/bin/env python3
# Extract bytes from DQ8.code for a guest address and disassemble them.
# Usage: python3 tools/dq8_disasm_at.py 0x002F9334 [count] [mode]
import subprocess, sys

addr = int(sys.argv[1], 16)
count = int(sys.argv[2]) if len(sys.argv) > 2 else 32
mode = sys.argv[3] if len(sys.argv) > 3 else "arm"
off = addr - 0x00100000
with open("/home/hermes/DQ8/DQ8.code", "rb") as fh:
    fh.seek(off)
    data = fh.read(count * 4)
out = "/tmp/dq8-sess/at_%08X.bin" % addr
with open(out, "wb") as fh:
    fh.write(data)
subprocess.run(["/home/hermes/DQ8/target/release/dq8tool", "disasm", out,
                "--mode", mode, "--base", "0x%08X" % addr, "--count", str(count)])
