#!/usr/bin/env python3
"""Slice DQ8.code at a guest address and disassemble it.

dq8tool's `disasm` always reads from the START of the file (--base only labels
addresses), so a targeted disassembly needs the bytes extracted first.
usage: tools/disasm_at.py 0xADDR [mode] [count]
"""
import subprocess
import sys

CODE = "DQ8.code"
TOOL = "./target/release/dq8tool"
TEXT = 0x00100000


def disasm(addr, mode, count):
    off = addr - TEXT
    with open(CODE, "rb") as f:
        f.seek(off)
        data = f.read(0x200)
    path = "/tmp/disasm_at_%x.bin" % addr
    with open(path, "wb") as f:
        f.write(data)
    r = subprocess.run([TOOL, "disasm", path, "--mode", mode,
                        "--base", hex(addr), "--count", str(count)],
                       capture_output=True, text=True)
    return (r.stdout or r.stderr).rstrip()


def main():
    addr = int(sys.argv[1], 0)
    modes = [sys.argv[2]] if len(sys.argv) > 2 and sys.argv[2] in ("arm", "thumb") else ["arm", "thumb"]
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    if len(sys.argv) > 2 and sys.argv[2] not in ("arm", "thumb"):
        count = int(sys.argv[2])
    for m in modes:
        print("===== %s @ 0x%08X =====" % (m, addr))
        print(disasm(addr, m, count))


if __name__ == "__main__":
    main()
