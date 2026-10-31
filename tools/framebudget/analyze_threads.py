#!/usr/bin/env python3
"""t_432a85ab: per-thread CPU + wchan share analysis.

Input: tsample output lines: ts tid comm utime stime wchan
"""
import sys, collections

path = sys.argv[1]
rows = []
with open(path) as f:
    for line in f:
        p = line.split()
        if len(p) < 6 or not p[1].isdigit():
            continue
        ts = float(p[0]); tid = int(p[1]); comm = p[2]
        ut, st = p[3], p[4]; wch = p[5]
        if ut == '?' or st == '?':
            continue
        rows.append((ts, tid, comm, int(ut), int(st), wch))

if not rows:
    sys.exit("no rows")

t0 = rows[0][0]; t1 = rows[-1][0]
# per-tid first/last
first = {}; last = {}
for r in rows:
    first.setdefault(r[1], r)
    last[r[1]] = r

def group(comm, tid):
    if comm.startswith('llvmpipe'):
        return 'llvmpipe (PICA/GL raster worker)'
    if comm == 'dq8-tex:disk$0':
        return 'guest disk thread'
    if comm == 'SDLHotplugALSA':
        return 'SDL hotplug (idle)'
    return 'other dq8 threads'

# main thread = the tid whose comm is dq8-texcache3 with highest cpu
main_tid = None; best = -1
for tid, r in last.items():
    if r[2] == 'dq8-texcache3':
        cpu = (r[3] + r[4]) - (first[tid][3] + first[tid][4])
        if cpu > best:
            best = cpu; main_tid = tid

# wchan distribution per group, weighted by samples
wchan_by_group = collections.defaultdict(collections.Counter)
cpu_by_group = collections.defaultdict(int)
n_samples_by_group = collections.defaultdict(int)
tid_label = {}
for tid, r in last.items():
    lab = 'MAIN thread (tid %d)' % tid if tid == main_tid else group(r[2], tid)
    tid_label[tid] = lab

for tid, r in first.items():
    lr = last[tid]
    lab = tid_label[tid]
    cpu_by_group[lab] += (lr[3] + lr[4]) - (r[3] + r[4])

for r in rows:
    lab = tid_label.get(r[1])
    if lab is None:
        continue
    n_samples_by_group[lab] += 1
    wchan_by_group[lab][r[5]] += 1

total_cpu = sum(cpu_by_group.values())
wall = t1 - t0
hz = 100.0
print("samples: %d  wall_window: %.1f s  (ticks are USER_HZ=100)" % (len(rows), wall))
print("total CPU across threads: %.1f s  (%.1f%% of one core, %.1f cores avg over window)"
      % (total_cpu / hz, 100.0 * total_cpu / hz / wall, total_cpu / hz / wall))
print()
print("%-38s %10s %8s %8s" % ("group", "cpu_s", "%cpu", "%wall"))
for lab, c in sorted(cpu_by_group.items(), key=lambda kv: -kv[1]):
    print("%-38s %10.1f %7.1f%% %7.1f%%" % (lab, c / hz, 100.0 * c / total_cpu, 100.0 * c / hz / wall))
print()
print("wchan (blocked) share by group — fraction of 0.1-1.5s samples observed blocked:")
for lab, ctr in sorted(wchan_by_group.items()):
    n = n_samples_by_group[lab]
    if n == 0:
        continue
    top = ", ".join("%s:%.0f%%" % (w, 100.0 * v / n) for w, v in ctr.most_common(4))
    run = 100.0 * ctr.get('0', 0) / n
    print("  %-38s n=%-5d running=%5.1f%%  %s" % (lab, n, run, top))
