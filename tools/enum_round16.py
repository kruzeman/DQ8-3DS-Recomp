#!/usr/bin/env python3
"""Round-16 comprehensive enumeration of unrooted code-like pointers.

Sources:
  A. All 4-byte-aligned words in the .rodata table region that look like code
     pointers (0x00100000..0x005FA000) and are NOT in recomp-roots.txt.
  B. .text pc-relative LDR literal callback targets not in recomp-roots.txt
     (scan_literal_callbacks logic, inlined).

Bucket filter: keep only safe entry shapes (push/str-lr/sub-sp prologues,
ldr-getter, mov-imm, bx-lr, cmp-stub, tail-jump branch).  Drop OTHER /
mid-function labels (add-imm, str/ldr byte, etc.) which cause recompile
runaway.

usage: tools/enum_round16.py [--out FILE]
"""
import glob
import re
import struct
import sys

CODE = '/home/hermes/DQ8/DQ8.code'
ROOTS = '/home/hermes/DQ8/recomp-roots.txt'
BASE = 0x00100000
TEXT_END = 0x0053BCB4
RODATA_SCAN_END = 0x005FA000

code = open(CODE, 'rb').read()
roots = set()
for line in open(ROOTS):
    line = line.strip()
    if line[:2] in ('0x', '0X'):
        try:
            roots.add(int(line, 16))
        except ValueError:
            pass

# already-translated cases (from a prior split, if present) — belt and braces
cases = set()
for p in glob.glob('/home/hermes/DQ8/target/dq8-split/dq8_part_*.c'):
    with open(p, errors='ignore') as fh:
        for line in fh:
            if line.startswith('case 0x'):
                m = re.match(r'case (0x[0-9A-F]+)', line)
                if m:
                    cases.add(int(m.group(1), 16))


def gw(a):
    o = a - BASE
    if o < 0 or o + 4 > len(code):
        return None
    return struct.unpack_from('<I', code, o)[0]


def bucket(w):
    """Return a safe-entry bucket name, or None to drop."""
    top = w & 0xFFFF0000
    if top == 0xE92D0000:                       # stmdb push {...}
        return 'push'
    if top == 0xE52DE000:                       # str lr, [sp, #-]!
        return 'str-lr'
    if (w & 0xFFF00000) == 0xE24DD000:          # sub sp, sp, #imm
        return 'sub-sp'
    if (w & 0xFFF00000) == 0xE5900000:          # ldr rd, [rn]  (getter)
        return 'ldr-getter'
    if (w & 0xFFFF0000) == 0xE59F0000:          # ldr rd, [pc, #+]
        return 'ldr-pc'
    if w == 0xE12FFF1E:                         # bx lr
        return 'bx-lr'
    if (w & 0xFFF00000) == 0xE3A00000:          # mov rd, #imm
        return 'mov-imm'
    if (w & 0xFFF00000) == 0xE3E00000:          # mvn rd, #imm (init-leaf
        return 'mvn-imm'                         #   family: esc16/17 entries)
    if (w & 0xFFB00000) == 0xED900000:          # vldr rd, [rn,#imm] / [pc]
        return 'vldr'
    if (w & 0xFFF00000) == 0xE3500000:          # cmp rd, #imm
        return 'cmp-stub'
    if (w & 0x0F000000) == 0x0A000000:          # b <imm>  (tail-jump)
        return 'branch'
    if (w & 0xFFE00000) == 0xE1A00000:          # mov/and/orr reg forms
        return 'mov-reg'
    return None


cands = {}

def add(t, src, force=False):
    if t in roots or t in cases:
        return
    if not (BASE <= t < TEXT_END) or t % 4:
        return
    w = gw(t)
    if w is None:
        return
    b = bucket(w)
    if b is None:
        if not force:
            return
        b = 'rodata-any'     # dense-table pointer: accept any code-range word;
                             # fail-fast bisect drops runaways / bad entries
    cands.setdefault(t, {'bucket': b, 'srcs': set()})['srcs'].add(src)

# Source A: .rodata table region, 4-byte stride — keep a word if it sits in a
# DENSE cluster of code-like pointers (a real vtable/pointer table).  Density
# (>=3 code-like words within +/-0x40) is the strong signal here; genuine method
# tables hold entries of every prologue shape (mov/mvn/vldr/push/getter/...), so
# we do NOT require a specific first-instruction bucket for this source —
# tools/bisect_runaway.sh fail-fast drops any entry that runaways.
def codelike(w):
    return w is not None and BASE <= w < 0x005FA000 and w % 4 == 0

for a in range(TEXT_END, RODATA_SCAN_END, 4):
    w = gw(a)
    if not codelike(w):
        continue
    # density check: count code-like words in [a-0x40, a+0x40]
    dens = 0
    for b in range(a - 0x40, a + 0x44, 4):
        if codelike(gw(b)):
            dens += 1
    if dens < 3:
        continue
    add(w, 'rodata@0x%08X' % a, force=True)

# Source B: .text ldr-literal callback targets
for a in range(BASE, TEXT_END, 4):
    w = gw(a)
    if w is None:
        continue
    if (w & 0x0F7F0000) != 0x051F0000:
        continue
    imm = w & 0xFFF
    dest = a + 8 + imm if (w >> 23) & 1 else a + 8 - imm
    if not (BASE <= dest < TEXT_END) or dest % 4:
        continue
    t = gw(dest)
    if t is None or not (BASE <= t < TEXT_END) or t % 4:
        continue
    add(t, 'literal@0x%08X[lr%d]' % (a, (w >> 12) & 0xF))

# report by bucket
by_bucket = {}
for t, info in cands.items():
    by_bucket.setdefault(info['bucket'], []).append(t)

print('unrooted safe-bucket candidates: %d' % len(cands))
for b in sorted(by_bucket, key=lambda x: -len(by_bucket[x])):
    print('  %-12s %d' % (b, len(by_bucket[b])))

out = []
for b in ('push', 'str-lr', 'sub-sp', 'ldr-getter', 'ldr-pc', 'bx-lr',
          'mov-imm', 'mvn-imm', 'vldr', 'cmp-stub', 'branch', 'mov-reg',
          'rodata-any'):
    for t in sorted(by_bucket.get(b, [])):
        out.append('0x%08X' % t)

outpath = None
for i, a in enumerate(sys.argv):
    if a == '--out' and i + 1 < len(sys.argv):
        outpath = sys.argv[i + 1]
if outpath:
    with open(outpath, 'w') as fh:
        fh.write('\n'.join(out) + '\n')
    print('wrote %d entries to %s' % (len(out), outpath))
