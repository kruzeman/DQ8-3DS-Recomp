#!/usr/bin/env python3
"""Scan DQ8.code for `str rX, [rY, #0xC]` (state-field style stores) whose
source register is set to a given immediate value shortly before.

Usage: scan_state_stores.py <imm> [start_guest_addr] [end_guest_addr]
Base = 0x00100000.
"""
import struct
import sys
import subprocess

IMM = int(sys.argv[1], 16) if sys.argv[1].lower().startswith('0x') else int(sys.argv[1])
START = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x00100000
END = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x7FFFFFFF
LOOK = 8

with open('/home/hermes/DQ8/DQ8.code', 'rb') as fh:
    data = fh.read()
BASE = 0x00100000
n = len(data) // 4
words = struct.unpack('<%dI' % n, data[:n * 4])


def mov_imm_reg(w, imm):
    """ARM MOV immediate (cond=e, 00111010, rot=0) -> returns Rd or None."""
    if (w & 0xFFF00000) == 0xE3A00000 and ((w >> 8) & 0xF) == 0 and (w & 0xFF) == imm:
        rd = (w >> 12) & 0xF
        if rd != 15:
            return rd
    return None


hits = []
for i in range(n):
    pc = BASE + i * 4
    if not (START <= pc <= END):
        continue
    w = words[i]
    if (w & 0xFFF00000) != 0xE5800000:   # STR imm, cond=e, P=1 U=1 B=0 W=0 L=0
        continue
    if (w & 0xFFF) != 0x00C:
        continue
    rd = (w >> 12) & 0xF
    rn = (w >> 16) & 0xF
    if rd == 15 or rn == 15:
        continue
    for k in range(1, LOOK + 1):
        if i - k < 0:
            break
        if mov_imm_reg(words[i - k], IMM) == rd:
            hits.append((pc, rd, rn, pc - k * 4))
            break

print('str rN,[rM,#0xC] with rN := #%d nearby: %d hits' % (IMM, len(hits)))


def disasm(addr, count):
    off = addr - BASE
    blob = data[off:off + count * 4]
    tmp = '/tmp/dq8-sess/ss_%08X.bin' % addr
    with open(tmp, 'wb') as fh:
        fh.write(blob)
    out = subprocess.run(['/home/hermes/DQ8/target/release/dq8tool', 'disasm', tmp,
                          '--mode', 'arm', '--base', '0x%08X' % addr,
                          '--count', str(count)], capture_output=True, text=True)
    return out.stdout.strip().splitlines()


for pc, rd, rn, movpc in hits[:25]:
    print('---- store at 0x%08X  str r%d, [r%d, #0xC]  (mov at 0x%08X) ----' % (pc, rd, rn, movpc))
    for line in disasm(movpc, (pc - movpc) // 4 + 6):
        print('   ', line)
