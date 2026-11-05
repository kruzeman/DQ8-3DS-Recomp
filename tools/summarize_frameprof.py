import glob
import re
import statistics
import sys

d = sys.argv[1] if len(sys.argv) > 1 else 'target/bench-final'
pat = (r'FRAMEPROF-SUM frames=(\d+) fps=([\d.]+) '
       r'avg\(total/guest/gl/present/audio/idle\)=([\d.]+)/([\d.]+)/([\d.]+)/'
       r'([\d.]+)/([\d.]+)/([\d.]+)')
keys = ('frames', 'iters_s', 'total', 'guest', 'gl', 'present', 'audio', 'idle')
for arm in ('b30', 'legacy', 'composed', 'composed_scalar', 'wsurf', 'selftest'):
    vals = {}
    for p in sorted(glob.glob('%s/%s-r*.log' % (d, arm))):
        for line in open(p, errors='replace'):
            m = re.search(pat, line)
            if m:
                for k, v in zip(keys, m.groups()):
                    vals.setdefault(k, []).append(float(v))
    if not vals:
        continue
    print('== %s (%d summary windows)' % (arm, len(vals['total'])))
    for k in keys:
        print('   %-8s mean %6.2f  min %6.2f  max %6.2f' % (
            k, statistics.mean(vals[k]), min(vals[k]), max(vals[k])))
