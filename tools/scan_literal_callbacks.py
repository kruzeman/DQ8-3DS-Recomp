#!/usr/bin/env python3
"""Collect pc-relative LDR literal targets in .text that look like function
entries but are NOT in recomp-roots.txt (the literal-callback family scan).

usage: tools/scan_literal_callbacks.py [0xSTART] [0xEND] [--all]
Prints candidates with their first decoded ARM instruction.
"""
import struct
import subprocess
import sys

CODE = '/home/hermes/DQ8/DQ8.code'
ROOTS = '/home/hermes/DQ8/recomp-roots.txt'
TOOL = '/home/hermes/DQ8/target/release/dq8tool'
BASE = 0x00100000
TEXT_END = 0x0053BCB4

code = open(CODE, 'rb').read()
roots = set()
for line in open(ROOTS):
    line = line.strip()
    if line[:2] in ('0x', '0X'):
        try:
            roots.add(int(line, 16))
        except ValueError:
            pass

args = [a for a in sys.argv[1:] if not a.startswith('--')]
show_all = '--all' in sys.argv
start = int(args[0], 0) if len(args) > 0 else BASE
end = int(args[1], 0) if len(args) > 1 else TEXT_END

# plausible ARM function-entry first words
def plausible(w):
    top = w & 0xFFFF0000
    if top in (0xE92D0000, 0xE8BD0000):          # stmdb push / ldmia pop
        return True
    if (w & 0xFFFFFFFF) == 0xE12FFF1E:           # bx lr
        return True
    if (w & 0xFFF00000) == 0xE3A00000:           # mov rd, #imm
        return True
    if (w & 0xFFFF0000) == 0xE59F0000:           # ldr rd, [pc, #+]
        return True
    if (w & 0xFFFF0000) == 0xE24DD000:           # sub sp, sp, #imm
        return True
    if (w & 0xFFFF0000) == 0xE52DE000:           # str lr, [sp, #-]!
        return True
    if (w & 0xFFFF0000) == 0xE02DE000:           # (rare) add lr
        return True
    if (w & 0xFFE00000) == 0xE1A00000:           # mov/and/orr reg forms
        return True
    if (w & 0xFFFF0000) == 0xE59F0000:
        return True
    if (w & 0xFFF00000) == 0xE3500000:           # cmp rd, #imm (stub starts)
        return True
    if (w & 0xFFFF0000) == 0xE92D0000:
        return True
    if (w & 0xFFC00000) == 0xE2400000:           # sub rd, rn, #imm
        return True
    if (w & 0xFFFF0000) == 0xE1A00000:           # mov rd, rm
        return True
    return False

targets = {}
for a in range(start, end, 4):
    off = a - BASE
    w = struct.unpack_from('<I', code, off)[0]
    if (w & 0x0F7F0000) != 0x051F0000:
        continue
    imm = w & 0xFFF
    dest = a + 8 + imm if (w >> 23) & 1 else a + 8 - imm
    if not (BASE <= dest < TEXT_END) or dest % 4:
        continue
    t = struct.unpack_from('<I', code, dest - BASE)[0]
    if not (BASE <= t < TEXT_END) or t % 4:
        continue
    if t in roots:
        continue
    first = struct.unpack_from('<I', code, t - BASE)[0]
    if not plausible(first):
        continue
    targets.setdefault(t, []).append((dest, a, (w >> 12) & 0xF))

print('unrooted literal-callback candidates in 0x%08X..0x%08X: %d'
      % (start, end, len(targets)))

def decode(addr, n=1):
    with open('/tmp/dq8-lit.bin', 'wb') as fh:
        fh.write(code[addr - BASE:addr - BASE + 4 * n])
    out = subprocess.run([TOOL, 'disasm', '/tmp/dq8-lit.bin', '--mode', 'arm',
                          '--base', '0x%08X' % addr, '--count', str(n)],
                         capture_output=True, text=True)
    lines = [l for l in out.stdout.splitlines() if l.strip()]
    return lines[0].split(': ', 1)[-1] if lines else '(no decode)'

for t in sorted(targets):
    ldrs = targets[t][:3]
    print('0x%08X (from: %s)  %s' %
          (t, ','.join('0x%X[lr%d]' % (lit, rd) for lit, a, rd in ldrs),
           decode(t)))
