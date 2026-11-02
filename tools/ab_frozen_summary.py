#!/usr/bin/env python3
"""Summarise ab_cores.py JSONL evidence for the frozen-snapshot A/B."""
import json, sys, statistics

path = sys.argv[1]
rows = [json.loads(l) for l in open(path) if l.strip()]
if len(sys.argv) > 2 and sys.argv[2] == 'full':
    for r in rows:
        print(json.dumps({k: r[k] for k in ('label', 'mode', 'wall_s', 'fps', 'ms_per_frame',
                                            'insn_per_s_M', 'aggregate_insn_per_s_M',
                                            'proc_cpu_s', 'proc_main_thread_cpu_s',
                                            'proc_cores_gt10pct', 'proc_cpu_s_per_core',
                                            'cores_gt50', 'system_busy_cores_avg',
                                            'setup_ms', 'vert_ms', 'draw_ms') if k in r}))
    sys.exit(0)

def arm(r):
    return 'S1' if 'dq8-ab-s1' in r['label'] else 'S0'

for metric in ('insn_per_s_M', 'aggregate_insn_per_s_M', 'fps'):
    vals = {a: [r[metric] for r in rows if arm(r) == a and metric in r] for a in ('S0', 'S1')}
    if not vals['S0'] or not vals['S1']:
        continue
    print('%s: S0 med=%.3f n=%d  S1 med=%.3f n=%d  delta=%+.1f%%'
          % (metric, statistics.median(vals['S0']), len(vals['S0']),
             statistics.median(vals['S1']), len(vals['S1']),
             100.0 * (statistics.median(vals['S1']) / statistics.median(vals['S0']) - 1)))

print()
print('%-26s %8s %8s %8s %8s %7s' % ('run', 'insnM/s', 'aggM/s', 'mainCpu', 'procCpu', 'cores'))
for r in rows:
    print('%-26s %8.3f %8.3f %8.2f %8.2f %7d' % (
        r['label'], r.get('insn_per_s_M', 0), r.get('aggregate_insn_per_s_M', 0),
        r.get('proc_main_thread_cpu_s') or 0, r['proc_cpu_s'], r['proc_cores_gt10pct']))

# per-fixed-work CPU cost (cpu-seconds for the same retired guest work)
for metric in ('proc_cpu_s', 'proc_main_thread_cpu_s'):
    vals = {a: [r[metric] for r in rows if arm(r) == a and r.get(metric)] for a in ('S0', 'S1')}
    if vals['S0'] and vals['S1']:
        print('%s per 10M-step run: S0 med=%.2f  S1 med=%.2f  delta=%+.1f%%'
              % (metric, statistics.median(vals['S0']), statistics.median(vals['S1']),
                 100.0 * (statistics.median(vals['S1']) / statistics.median(vals['S0']) - 1)))

# paired ratios within each pair index
pairs = {}
for r in rows:
    idx = r['label'].rsplit('-', 1)[-1]
    pairs.setdefault(idx, {})[arm(r)] = r
print()
for idx in sorted(pairs):
    a = pairs[idx]
    if 'S0' in a and 'S1' in a:
        s0, s1 = a['S0'], a['S1']
        print('pair %s: S0 %.3fM vs S1 %.3fM insn/s = %+.1f%% | main_cpu %.2f vs %.2f = %+.1f%%'
              % (idx, s0.get('insn_per_s_M', 0), s1.get('insn_per_s_M', 0),
                 100.0 * (s1.get('insn_per_s_M', 0) / s0.get('insn_per_s_M', 1) - 1),
                 s0.get('proc_main_thread_cpu_s') or 0, s1.get('proc_main_thread_cpu_s') or 0,
                 100.0 * ((s1.get('proc_main_thread_cpu_s') or 0) /
                          max(s0.get('proc_main_thread_cpu_s') or 1, 1e-9) - 1)))
