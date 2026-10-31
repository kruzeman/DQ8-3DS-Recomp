#!/usr/bin/env python3
"""Diff a DQ8.code data window's code pointers against recomp-roots.txt.

usage: tools/scan_table_missing.py 0x0058E060 0x0058E160
Prints each pointer that is NOT yet a root, with its first decoded insn.
"""
import struct
import subprocess
import sys

BASE = 0x00100000
TEXT_END = 0x0053BCB4
DQ8_CODE = '/home/hermes/DQ8/DQ8.code'
ROOTS = '/home/hermes/DQ8/worktrees/audio/recomp-roots.txt'


def first_insn(addr):
    off = addr - BASE
    with open(DQ8_CODE, 'rb') as fh:
        fh.seek(off)
        data = fh.read(4)
    with open('/tmp/dq8-sess/scan_one.bin', 'wb') as fh:
        fh.write(data)
    out = subprocess.run(['/home/hermes/DQ8/target/release/dq8tool', 'disasm',
                          '/tmp/dq8-sess/scan_one.bin', '--mode', 'arm',
                          '--base', '0x%08X' % addr, '--count', '1'],
                         capture_output=True, text=True)
    lines = [l for l in out.stdout.strip().split('\n') if l.strip()]
    return lines[0] if lines else '(no decode)'


lo = int(sys.argv[1], 16)
hi = int(sys.argv[2], 16)
with open(DQ8_CODE, 'rb') as fh:
    fh.seek(lo - BASE)
    blob = fh.read(hi - lo)
with open(ROOTS) as fh:
    roots = {int(line.strip(), 16) for line in fh
             if line.strip() and not line.startswith('#')}

missing = []
seen = set()
for off in range(0, len(blob) - 3, 4):
    addr = lo + off
    word = struct.unpack_from('<I', blob, off)[0]
    if BASE <= word < TEXT_END and (word & 1) == 0 and word not in roots and word not in seen:
        seen.add(word)
        missing.append(word)

print(f'{len(missing)} unrooted code pointer(s) in [{lo:#010x}, {hi:#010x})')
for a in missing:
    print(f'  {a:#010x}: {first_insn(a)}')
