#!/usr/bin/env python3
"""Find BL sites to a target guest address in DQ8.code and show the few
preceding instructions (to recover the r1 immediate = new state).

Usage: scan_bl_calls.py 0x002FC018 [lookback]
Base guest address of .code text is 0x00100000.
"""
import struct
import sys
import subprocess

TARGET = int(sys.argv[1], 16)
LOOK = int(sys.argv[2]) if len(sys.argv) > 2 else 6

with open('/home/hermes/DQ8/DQ8.code', 'rb') as fh:
    data = fh.read()
BASE = 0x00100000
n = len(data) // 4
words = struct.unpack('<%dI' % n, data[:n * 4])

sites = []
for i in range(n):
    w = words[i]
    op = w & 0x0F000000
    if op not in (0x0A000000, 0x0B000000, 0x0A000000):  # B / BL, any condition
        continue
    imm = w & 0x00FFFFFF
    if imm & 0x800000:
        imm -= 0x1000000
    pc = BASE + i * 4
    dest = pc + 8 + imm * 4
    if dest == TARGET:
        cond = 'al' if (w >> 28) == 0xE else 'c%01X' % (w >> 28)
        sites.append((pc, 'BL' if op == 0x0B000000 else 'B', cond))

print('branch sites to 0x%08X: %d' % (TARGET, len(sites)))


def disasm(addr, count):
    off = addr - BASE
    blob = data[off:off + count * 4]
    tmp = '/tmp/dq8-sess/scan_%08X.bin' % addr
    with open(tmp, 'wb') as fh:
        fh.write(blob)
    out = subprocess.run(['/home/hermes/DQ8/target/release/dq8tool', 'disasm', tmp,
                          '--mode', 'arm', '--base', '0x%08X' % addr,
                          '--count', str(count)], capture_output=True, text=True)
    return out.stdout.strip().splitlines()


for pc, kind, cond in sites:
    start = pc - LOOK * 4
    print('---- %s%s at 0x%08X ----' % (kind, cond, pc))
    for line in disasm(start, LOOK + 1):
        print('   ', line)
