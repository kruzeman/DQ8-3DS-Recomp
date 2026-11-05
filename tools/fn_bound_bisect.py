#!/usr/bin/env python3
"""Bisect which emitted function breaks the deterministic run (t_21dda9b5).

Runs the emulator with DQ8_FN_EMIT=<comma list of entries>, compares the
GPU/PICA trace against the gate-off reference trace, and prints MATCH/DIFF per
chunk.  Usage:
    tools/fn_bound_bisect.py chunks [N]     # split the 300 into N chunks
    tools/fn_bound_bisect.py list 0x... ... # test an explicit list
"""
import os
import subprocess
import sys

ROOT = '/home/hermes/DQ8/worktrees/fnbound-t_21dda9b5'
DATA = '/home/hermes/DQ8/evidence/rx580-holes-20261101/scene-holes.sav'
BIN = os.path.join(ROOT, 'target/dq8-fnbound')
REF = os.path.join(ROOT, 'target/fn-bound-det/off.gpu')
OUT = os.path.join(ROOT, 'target/fn-bound-bisect')
os.makedirs(OUT, exist_ok=True)

ENV = dict(os.environ)
ENV.update({
    'DQ8_HEADLESS': '1', 'DQ8_GL': '1', 'DQ8_RUN_GUEST_THREADS': '1', 'DQ8_VSPAR': '4',
    'DQ8_SPLIT_GL': '0', 'DQ8_STACK_NO_RELOCATE': '1', 'DQ8_GL_DEPTH_ATTACH': '1',
    'DQ8_DETERMINISTIC': '1', 'DQ8_TRACE_GPU': '1', 'DQ8_BENCH_STEPS': '2000000',
    'DQ8_USER_DATA_DIR': os.path.join(ROOT, 'target/ud-bisect'),
})

with open(REF, 'rb') as f:
    ref = f.read()


def entries():
    out = []
    for line in open(os.path.join(ROOT, 'tools/fn_emit_top300.txt')):
        line = line.strip()
        if line and not line.startswith('#'):
            out.append(int(line.split()[0], 16))
    return out


def run(tag, gate):
    err = os.path.join(OUT, tag + '.err')
    with open(err, 'wb') as f:
        rc = subprocess.call(
            ['timeout', '-k', '5', '300', BIN, '--load-state', DATA, 'DQ8.code', 'dq8.romfs'],
            cwd=ROOT, env=dict(ENV, DQ8_FN_EMIT=gate), stdout=subprocess.DEVNULL, stderr=f)
    gpu = []
    for line in open(err, 'r', errors='replace'):
        if any(k in line for k in ('GPU', 'PICA ', 'draw', 'DRAW', 'XFER', 'TEX', 'SWAP')):
            gpu.append(line)
    got = ''.join(gpu).encode()
    return ('MATCH' if got == ref else 'DIFF'), len(gpu), rc


def main():
    mode = sys.argv[1]
    if mode == 'chunks':
        n = int(sys.argv[2]) if len(sys.argv) > 2 else 15
        es = entries()
        size = (len(es) + n - 1) // n
        for i in range(0, len(es), size):
            chunk = es[i:i + size]
            gate = ','.join('0x%08X' % e for e in chunk)
            verdict, lines, rc = run('chunk_%04d' % chunk[0], gate)
            print('%s chunk 0x%08X..0x%08X (%d fns) rc=%s lines=%d  %s'
                  % ('!', chunk[0], chunk[-1], len(chunk), rc, lines, verdict), flush=True)
    elif mode == 'auto':
        # recursive bisection: return the smallest failing subsets
        bad = []

        def bisect(es, depth=0):
            if not es:
                return
            gate = ','.join('0x%08X' % e for e in es)
            if len(es) == 1:
                verdict, lines, rc = run('b_%08X' % es[0], gate)
            else:
                verdict, lines, rc = run('b_%08X_%d' % (es[0], len(es)), gate)
            if verdict == 'MATCH':
                return
            if len(es) == 1:
                print('  FAIL 0x%08X rc=%s lines=%d' % (es[0], rc, lines), flush=True)
                bad.append(es[0])
                return
            mid = len(es) // 2
            before = len(bad)
            bisect(es[:mid], depth + 1)
            bisect(es[mid:], depth + 1)
            if len(bad) == before:
                print('  INTERACTION in %d fns 0x%08X..0x%08X' % (len(es), es[0], es[-1]), flush=True)

        bisect(entries())
        print('failing functions:', ' '.join('0x%08X' % e for e in bad))
    else:
        es = [int(x, 16) for x in sys.argv[2:]]
        gate = ','.join('0x%08X' % e for e in es)
        verdict, lines, rc = run('list', gate)
        print('list %s rc=%s lines=%d %s' % (gate[:120], rc, lines, verdict))


main()
