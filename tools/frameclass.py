import re, statistics, sys
path = sys.argv[1] if len(sys.argv) > 1 else '/tmp/xfer-run/base.err'
rows = []
for l in open(path):
    if l.startswith('FRAMEPROF f='):
        d = dict(re.findall(r'(\w+)=([\d.]+)', l))
        rows.append({k: float(v) for k, v in d.items()})
print('frames', len(rows))
cls = {'fast': [], 'mid': [], 'spike': []}
for r in rows:
    t = r['total_ms']
    k = 'fast' if t < 30 else ('mid' if t < 100 else 'spike')
    cls[k].append(r)
for k, v in cls.items():
    if not v:
        continue
    print(k, len(v),
          'total %.1f' % statistics.mean(x['total_ms'] for x in v),
          'guest %.1f' % statistics.mean(x['guest_ms'] for x in v),
          'gl %.1f' % statistics.mean(x['gl_ms'] for x in v),
          'pres %.1f' % statistics.mean(x['present_ms'] for x in v),
          'idle %.1f' % statistics.mean(x['idle_ms'] for x in v))
print('last 16 totals:', [round(r['total_ms'], 1) for r in rows[-16:]])
print('spike samples:')
for r in [x for x in rows if x['total_ms'] > 100][:6]:
    print({k: round(r[k], 1) for k in ('f', 'total_ms', 'guest_ms', 'gl_ms', 'present_ms', 'idle_ms')})
# cadence: index of heavy frames
heavy = [i for i, r in enumerate(rows) if r['total_ms'] > 30]
diffs = [b - a for a, b in zip(heavy, heavy[1:])]
from collections import Counter
print('heavy count', len(heavy), 'cadence', Counter(diffs).most_common(5))
