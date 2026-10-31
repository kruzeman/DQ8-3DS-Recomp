#!/usr/bin/env python3
"""Disasm-verify missing-root candidates in a table window.

For every aligned word in [start,end) that looks like a .text pointer and is
absent from recomp-roots.txt, decode its first two ARM instructions with
dq8tool so each entry can be validated as a plausible function entry before
being appended to the root list.
usage: tools/verify_batch_entries.py 0xSTART 0xEND
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

start = int(sys.argv[1], 0)
end = int(sys.argv[2], 0)

def gw(a):
    o = a - BASE
    if o < 0 or o + 4 > len(code):
        return None
    return struct.unpack_from('<I', code, o)[0]

def decode(addr, n=2):
    off = addr - BASE
    blob = '/tmp/dq8-verify-entry.bin'
    with open(blob, 'wb') as fh:
        fh.write(code[off:off + 4 * n])
    out = subprocess.run([TOOL, 'disasm', blob, '--mode', 'arm',
                          '--base', '0x%08X' % addr, '--count', str(n)],
                         capture_output=True, text=True)
    lines = [l for l in out.stdout.splitlines() if l.strip()]
    return ' ; '.join(l.split(': ', 1)[-1] for l in lines[:n]) or '(no decode)'

seen = set()
bad = []
for a in range(start & ~3, end, 4):
    w = gw(a)
    if w is None or w in roots or w in seen:
        continue
    if not (BASE <= w < TEXT_END and w % 4 == 0):
        continue
    seen.add(w)
    ins = decode(w)
    # crude data smells: undefined encodings or literal-pool style words
    smell = ''
    if ins.startswith('(no decode)') or 'unsupported' in ins:
        smell = '  <<< SUSPECT'
        bad.append(w)
    print('0x%08X @ %08X  %s%s' % (w, a, ins, smell))

print('')
print('candidates=%d suspects=%d' % (len(seen), len(bad)))
if bad:
    print('SUSPECTS: ' + ' '.join('0x%08X' % b for b in bad))
