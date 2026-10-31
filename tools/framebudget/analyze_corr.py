#!/usr/bin/env python3
"""Validate: per-round CPU delta vs the sampled RIP bucket (bias check)."""
import sys, os, collections
base = '/home/hermes/.hermes/kanban/workspaces/t_432a85ab/evidence/' + sys.argv[1]

samples = {}
with open(os.path.join(base, 'samples.tsv')) as f:
    next(f)
    for line in f:
        p = line.rstrip('\n').split('\t')
        samples[(round(float(p[0]), 3), int(p[1]))] = (p[4], p[5])

proc = []
with open(os.path.join(base, 'proc.tsv')) as f:
    next(f)
    for line in f:
        p = line.rstrip('\n').split('\t')
        if len(p) < 7:
            continue
        proc.append((float(p[0]), int(p[1]), p[2], int(p[3]) + int(p[4]), p[5]))

# group rounds by timestamp
rounds = collections.defaultdict(dict)
for ts, tid, comm, cpu, wch in proc:
    rounds[round(ts, 3)][tid] = (cpu, wch)

def classify(mod, sym, comm, wch):
    if comm.startswith('llvmpipe'):
        if sym.startswith('<libc.so.6+0x181>') or 'futex' in sym:
            return 'llvmpipe: futex'
        if mod == '?':
            return 'llvmpipe: JIT(anon)'
        if mod.startswith('libgallium') or mod.startswith('libLLVM'):
            return 'llvmpipe: %s' % mod[:20]
        return 'llvmpipe: %s %s' % (mod, sym[:30])
    return None

keys = sorted(rounds)
comm_of = {}
for r in proc:
    comm_of.setdefault(r[1], r[2])
agg = collections.defaultdict(lambda: [0, 0])  # key -> [samples, cpu_ticks_sum]
for i in range(1, len(keys)):
    prev, cur = keys[i - 1], keys[i]
    for tid, (cpu0, _) in rounds[prev].items():
        if tid not in rounds[cur]:
            continue
        cpu1, wch = rounds[cur][tid]
        d = cpu1 - cpu0
        s = samples.get((cur, tid))
        if not s:
            continue
        comm = comm_of.get(tid, '')
        c = classify(s[0], s[1], comm, wch)
        if c is None:
            if comm == 'dq8-texcache3':
                c = 'MAIN: %s' % ('futex' if s[1].startswith('<libc.so.6+0x181>') or 'futex' in s[1]
                                  else ('PICA/GL' if s[1].startswith(('pica_', 'pvs_', 'pgl_', 'gsp_'))
                                        else ('guest' if s[1].startswith('dq8_part_') else s[1][:24])))
            else:
                continue
        agg[c][0] += 1
        agg[c][1] += d

print('%-34s %7s %10s %10s' % ('bucket', 'samples', 'cpu_s', 'cpu_s/sample'))
for k, (n, ticks) in sorted(agg.items(), key=lambda kv: -kv[1][1]):
    print('%-34s %7d %10.2f %10.3f' % (k, n, ticks / 100.0, ticks / 100.0 / max(n, 1)))
