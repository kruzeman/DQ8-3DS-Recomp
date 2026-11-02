#!/usr/bin/env python3
"""Per-run fps / CPU table for the frozen A/B, incl. guest progress markers."""
import glob, json, os, re, sys

base = sys.argv[1] if len(sys.argv) > 1 else 'evidence/t_03f3f620'
rows = []
for d in sorted(glob.glob(os.path.join(base, '*' + os.sep + 'sample.json'))):
    r = json.load(open(d))
    log = os.path.join(os.path.dirname(d), 'run.log')
    steps = None
    if os.path.exists(log):
        m = re.findall(r'steps=(\d+)', open(log, errors='replace').read())
        if m:
            steps = max(int(x) for x in m)
    r['guest_steps'] = steps
    rows.append(r)

hdr = ('%-22s %7s %7s %9s %9s %10s %10s %8s'
       % ('run', 'fps', 'frames', 'main_cpu_s', 'proc_cpu_s', 'cpu/frame', 'guest_M', 'ns/insn'))
print(hdr)
for r in rows:
    fr = r.get('frames') or 0
    m = r.get('proc_main_thread_cpu_s') or 0
    g = r.get('guest_steps') or 0
    print('%-22s %7.3f %7d %9.2f %9.2f %10.1f %10.1f %8.1f'
          % (r['label'], r.get('fps') or 0, fr, m, r['proc_cpu_s'],
             (1000.0 * m / fr) if fr else 0, g / 1e6 if g else 0,
             (1e9 * m / g) if g else 0))

arms = {'S0': [], 'S1': []}
for r in rows:
    a = 'S1' if 'dq8-ab-s1' in r['label'] else 'S0'
    arms[a].append(r)
print()
for a, rs in arms.items():
    if not rs:
        continue
    fps = [r['fps'] for r in rs if r.get('fps')]
    cpf = [1000.0 * (r.get('proc_main_thread_cpu_s') or 0) / r['frames']
           for r in rs if r.get('frames')]
    nsi = [1e9 * (r.get('proc_main_thread_cpu_s') or 0) / r['guest_steps']
           for r in rs if r.get('guest_steps')]
    import statistics as st
    print('%s n=%d fps med=%.3f  main cpu/frame med=%.1f ms  main ns/guest-insn med=%.1f' %
          (a, len(rs), st.median(fps) if fps else 0,
           st.median(cpf) if cpf else 0, st.median(nsi) if nsi else 0))
