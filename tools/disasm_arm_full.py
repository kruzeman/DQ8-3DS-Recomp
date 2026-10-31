#!/usr/bin/env python3
# Full ARM (incl. VFP/NEON) disassembly of a guest range from DQ8.code via llvm-objdump.
# Usage: python3 tools/disasm_arm_full.py 0x003D6F70 0x003D7360 [thumb]
import subprocess, sys, os

start = int(sys.argv[1], 16)
end = int(sys.argv[2], 16) if len(sys.argv) > 2 else start + 0x400
mode = sys.argv[3] if len(sys.argv) > 3 else "arm"
off = start - 0x00100000
with open("/home/hermes/DQ8/DQ8.code", "rb") as fh:
    fh.seek(off)
    data = fh.read(end - start)
os.makedirs("/tmp/dq8-sess", exist_ok=True)
out = "/tmp/dq8-sess/range_%08X_%08X.bin" % (start, end)
with open(out, "wb") as fh:
    fh.write(data)
cmd = ["llvm-objdump", "-D", "-b", "binary", "--mcpu=cortex-a9",
       "--mattr=+vfp3,+NEON" if mode == "arm" else "--mattr=+vfp3,+NEON,+thumb2",
       "-m", "arm" if mode == "arm" else "thumb",
       "--adjust-vma=0x%08X" % start, out]
subprocess.run(cmd)
