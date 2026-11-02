#!/usr/bin/env python3
"""Per-core utilisation comparison (S0 vs S1) from ab_cores.py samples."""
import glob, json, os, statistics as st, sys

base = sys.argv[1] if len(sys.argv) > 1 else 'evidence/t_03f3f620'
arms = {'S0': [], 'S1': []}
for d in sorted(glob.glob(os.path.join(base, 'fps-*/sample.json'))):
    r = json.load(open(d))
    arms['S1' if 'dq8-ab-s1' in r['label'] else 'S0'].append(r)

for a, rs in arms.items():
    if not rs:
        continue
    walls = [r['wall_s'] for r in rs]
    cores = {}
    for r in rs:
        for cpu, s in r['proc_cpu_s_per_core'].items():
            cores.setdefault(int(cpu), []).append(s)
    mean_s = {c: st.mean(v) for c, v in cores.items()}
    wall = st.mean(walls)
    busy = sorted(mean_s.values(), reverse=True)
    print('%s: runs=%d wall=%.1fs  proc_cpu=%.1fs/run -> %.2f cores busy'
          % (a, len(rs), wall, st.mean([r['proc_cpu_s'] for r in rs]),
             st.mean([r['proc_cpu_s'] for r in rs]) / wall))
    print('   main thread: %.1f s/run = %.0f%% duty | fps %.3f | main CPU/frame %.1f ms'
          % (st.mean([r['proc_main_thread_cpu_s'] for r in rs]),
             100 * st.mean([r['proc_main_thread_cpu_s'] for r in rs]) / wall,
             st.median([r['fps'] for r in rs]),
             st.median([1000.0 * r['proc_main_thread_cpu_s'] / r['frames'] for r in rs])))
    print('   our-process CPU-seconds per core (all 24, descending): %s'
          % ' '.join('%.1f' % v for v in busy))
    print('   cores with >=1.0 s of our CPU: %d ; >=3.0 s: %d ; >=5.0 s: %d ; max single core %.1f s (%.0f%% of wall)'
          % (sum(1 for v in mean_s.values() if v >= 1.0),
             sum(1 for v in mean_s.values() if v >= 3.0),
             sum(1 for v in mean_s.values() if v >= 5.0), busy[0], 100 * busy[0] / wall))
    print('   system-wide (contaminated by co-tenants): %.2f busy cores avg, cores>=50%%: %.1f'
          % (st.mean([r['system_busy_cores_avg'] for r in rs]),
             st.mean([r['cores_gt50'] for r in rs])))
    print()
