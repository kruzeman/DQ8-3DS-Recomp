#!/usr/bin/env python3
# Clean ARM disassembly of a DQ8.code range.
# Prefers capstone if installed; falls back to dq8tool's disasm.
# Usage: python3 tools/disasm_clean.py 0xADDR 0xEND
import subprocess, sys

CODE = "/home/hermes/DQ8/DQ8.code"
lo = int(sys.argv[1], 16)
hi = int(sys.argv[2], 16)
with open(CODE, "rb") as f:
    f.seek(lo - 0x100000)
    data = f.read(hi - lo)

try:
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    for insn in md.disasm(data, lo):
        print("0x%08X: %-8s %s %s" % (insn.address, insn.bytes.hex(), insn.mnemonic, insn.op_str))
    sys.exit(0)
except ImportError:
    pass

open("/tmp/dq8-sess/clean.bin", "wb").write(data)
r = subprocess.run(
    ["/home/hermes/DQ8/target/release/dq8tool", "disasm", "/tmp/dq8-sess/clean.bin",
     "--mode", "arm", "--base", "0x%08X" % lo, "--count", str((hi - lo) // 4)],
    capture_output=True, text=True)
print(r.stdout)
if r.stderr:
    sys.stderr.write(r.stderr)
