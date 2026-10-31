#!/usr/bin/env python3
"""Resolve the 8 callback literals feeding the 0x004D63FC message builder."""
import struct
import subprocess
import sys

CODE = '/home/hermes/DQ8/DQ8.code'
ROOTS = '/home/hermes/DQ8/recomp-roots.txt'
TOOL = '/home/hermes/DQ8/target/release/dq8tool'
BASE = 0x00100000

code = open(CODE, 'rb').read()
roots = set()
for line in open(ROOTS):
    line = line.strip()
    if line[:2] in ('0x', '0X'):
        try:
            roots.add(int(line, 16))
        except ValueError:
            pass

loaders = [0x001B6B1C, 0x001B726C, 0x001B73DC, 0x001B7460,
           0x001B76B8, 0x001B79D8, 0x001B7C48, 0x001B83E8]

def decode(addr, n=3):
    with open('/tmp/dq8-fam.bin', 'wb') as fh:
        fh.write(code[addr - BASE:addr - BASE + 4 * n])
    out = subprocess.run([TOOL, 'disasm', '/tmp/dq8-fam.bin', '--mode', 'arm',
                          '--base', '0x%08X' % addr, '--count', str(n)],
                         capture_output=True, text=True)
    lines = [l for l in out.stdout.splitlines() if l.strip()]
    return ' ; '.join(l.split(': ', 1)[-1] for l in lines[:n]) or '(no decode)'

missing = []
for ld in loaders:
    w = struct.unpack_from('<I', code, ld - BASE)[0]
    imm = w & 0xFFF
    lit = ld + 8 + imm if (w >> 23) & 1 else ld + 8 - imm
    val = struct.unpack_from('<I', code, lit - BASE)[0]
    inroots = val in roots
    status = 'ROOTED' if inroots else 'MISSING'
    if not inroots:
        missing.append(val)
    print('loader 0x%08X -> lit 0x%08X = 0x%08X [%s]  first: %s'
          % (ld, lit, val, status, decode(val)))

print('')
print('missing: %d  %s' % (len(missing), ' '.join('0x%08X' % m for m in missing)))
