#!/usr/bin/env python3
"""tools/analyze_present.py -- summarise present_bench.sh logs (t_5286064e).

Per arm (label = the binary/knob under test), over all its runs:
  present_ms/iter   mean/p50/p90/max of the per-iteration FRAMEPROF present
                    phase (the card's primary metric: <= 0.7 ms mean)
  present/frame     present_ms mean * iterations-per-second / published-fps,
                    i.e. the card's "ms per PUBLISHED frame" (was ~5.4 ms)
  guest_ms/gl_ms    per iteration, so a build difference cannot hide behind
                    a present win
  fps               FPSLOG fps= (guest buffer flips per wall second, the
                    honest counter) and TEMPO swap_per_s
  composed          the "present composed:" counter line

usage: tools/analyze_present.py OUTDIR [OUTDIR...]
"""
import os
import re
import sys
import statistics

FRE = re.compile(r'FRAMEPROF f=\d+ .*?total_ms=([\d.]+) guest_ms=([\d.]+) '
                 r'gl_ms=([\d.]+) present_ms=([\d.]+)')
SUMM = re.compile(r'FRAMEPROF-SUM frames=(\d+) fps=([\d.]+) '
                  r'avg\(total/guest/gl/present/audio/idle\)=([\d.]+)/([\d.]+)/'
                  r'([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+)')
FPS = re.compile(r'FPSLOG fps=([\d.]+)/s.*?frame_ms=([\d.]+)')
SWAP = re.compile(r'TEMPO .*?swap_per_s=([\d.]+)')
COMP = re.compile(r'present composed: presents=(\d+) screens_composed=(\d+) '
                  r'unchanged_skips=(\d+) translucent_redos=(\d+)'
                  r'(?: ms_compose/present=([\d.]+) ms_put/present=([\d.]+))?')


def pct(v, q):
    s = sorted(v)
    i = min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))
    return s[i]


def stat(v):
    if not v:
        return "n/a"
    return "mean %6.2f  p50 %6.2f  p90 %6.2f  max %6.2f  (n=%d)" % (
        statistics.mean(v), pct(v, .5), pct(v, .9), max(v), len(v))


def collect(path):
    out = dict(present=[], guest=[], gl=[], fps=[], swap=[], comps=[],
               sum_frames=0.0, sum_elapsed=0.0)
    with open(path, errors='replace') as f:
        for line in f:
            m = FRE.search(line)
            if m:
                out['present'].append(float(m.group(4)))
                out['guest'].append(float(m.group(2)))
                out['gl'].append(float(m.group(3)))
                continue
            m = SUMM.search(line)
            if m and float(m.group(2)) > 0:
                out['sum_frames'] += float(m.group(1))
                out['sum_elapsed'] += float(m.group(1)) / float(m.group(2))
                continue
            m = FPS.search(line)
            if m:
                out['fps'].append(float(m.group(1)))
                continue
            m = SWAP.search(line)
            if m:
                out['swap'].append(float(m.group(1)))
                continue
            m = COMP.search(line)
            if m:
                g = m.groups()
                out['comps'].append((int(g[0]), int(g[1]), int(g[2]), int(g[3]),
                                     g[4], g[5]))
    return out


def main(dirs):
    for d in dirs:
        arms = {}
        for name in sorted(os.listdir(d)):
            if not name.endswith('.log'):
                continue
            label, _, run = name[:-4].rpartition('-r')
            if not run.isdigit():
                continue
            arms.setdefault(label, []).append(os.path.join(d, name))
        print("=" * 78)
        print("dir:", d)
        for label in sorted(arms):
            runs = [collect(p) for p in sorted(arms[label])]
            pres = [v for r in runs for v in r['present']]
            guest = [v for r in runs for v in r['guest']]
            gl = [v for r in runs for v in r['gl']]
            fps = [v for r in runs for v in r['fps']]
            swap = [v for r in runs for v in r['swap']]
            print("\n-- %s  (%d runs)" % (label, len(runs)))
            print("   present_ms/iter : %s" % stat(pres))
            print("   guest_ms/iter   : %s" % stat(guest))
            print("   gl_ms/iter      : %s" % stat(gl))
            if fps:
                print("   FPSLOG fps      : %s" % stat(fps))
            if swap:
                print("   TEMPO swap/s    : %s" % stat(swap))
            for r, p in zip(runs, sorted(arms[label])):
                n = len(r['present'])
                if not n:
                    print("     %-20s no FRAMEPROF lines" % os.path.basename(p))
                    continue
                el = r['sum_elapsed']
                it_s = (r['sum_frames'] / el) if el > 0 else 0.0
                f = statistics.mean(r['fps']) if r['fps'] else 0.0
                pm = statistics.mean(r['present'])
                per_pub = (pm * it_s / f) if f else 0.0
                print("     %-20s iters=%5d it/s=%5.1f fps=%5.2f "
                      "present_mean=%5.2f ms/frame=%5.2f" % (
                          os.path.basename(p), n, it_s, f, pm, per_pub))
            for c in runs[0]['comps']:
                print("   composed counters: presents=%d screens=%d "
                      "unchanged_skips=%d translucent_redos=%d%s" % (
                          c[0], c[1], c[2], c[3],
                          ("  ms_compose/present=%.3f ms_put/present=%.3f"
                           % (float(c[4]), float(c[5]))) if c[4] else ""))


if __name__ == '__main__':
    main(sys.argv[1:] or ['.'])
