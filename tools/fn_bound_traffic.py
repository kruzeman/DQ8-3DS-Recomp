#!/usr/bin/env python3
"""Static boundary-traffic statistics of the two emitted arms (t_21dda9b5):
mean prelude loads / epilogue stores per function, mean spill/reload ops per
native call site, and the totals over the 300-function list."""
import glob
import re
import sys

ROOT = '/home/hermes/DQ8/worktrees/fnbound-t_21dda9b5/'


def stats(d):
    loads, stores = [], []
    spills, reloads = [], []
    sites = 0
    for p in glob.glob(ROOT + d + '/dq8_fnshard_*.c'):
        txt = open(p).read()
        for line in txt.split('\n'):
            m = re.match(r' \* Exact slots.*loads (\d+) .*stores (\d+)', line)
            if m:
                loads.append(int(m.group(1)))
                stores.append(int(m.group(2)))
        for m in re.finditer(
                r'r14 = 0x[0-9A-F]+u;\n(.*?)st->pc = 0x[0-9A-F]+u;\n'
                r'(?:    if \(dq8_fn_stats_on\).*?\n)?    exec \+= .*?\n'
                r'    pc = st->pc;\n(.*?)\n    if \(pc ==', txt, re.S):
            spills.append(m.group(1).count('='))
            reloads.append(m.group(2).count('='))
            sites += 1
    n = len(loads) or 1
    s = len(spills) or 1
    return dict(fns=len(loads), load=sum(loads) / n, store=sum(stores) / n,
                sites=sites, spill=sum(spills) / s, reload=sum(reloads) / s,
                total_fn_ops=sum(loads) + sum(stores),
                total_site_ops=sum(spills) + sum(reloads))


for tag, d in (('exact', 'target/fnbound-split'), ('spill-all', 'target/fnbound-split-before')):
    st = stats(d)
    print('%-10s fns=%d mean_prelude=%.1f mean_epilogue=%.1f (sum %d) | sites=%d '
          'mean_spill=%.1f mean_reload=%.1f (sum %d)'
          % (tag, st['fns'], st['load'], st['store'], st['total_fn_ops'],
             st['sites'], st['spill'], st['reload'], st['total_site_ops']))
