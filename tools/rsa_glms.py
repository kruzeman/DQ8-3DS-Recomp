"""t_05281b89 stage A: gl_ms per frame from DQ8_FRAMEPROF logs.

Reads one or more run.log files (DQ8_FRAMEPROF=1) and prints, per run, the
number of frames, mean/median/p90 gl_ms over ALL frames, over the frames that
did GL work (gl_ms > 0) and the total GL time, plus the same for total_ms.

usage: rsa_glms.py <label>=<run.log> ...
"""
import sys


def stats(v):
    if not v:
        return (0.0, 0.0, 0.0, 0.0)
    v = sorted(v)
    n = len(v)
    mean = sum(v) / n
    med = v[n // 2]
    p90 = v[min(n - 1, int(n * 0.9))]
    return (mean, med, p90, sum(v))


def parse(path):
    gl, tot, gst, pres = [], [], [], []
    for line in open(path, errors='replace'):
        if not line.startswith('FRAMEPROF'):
            continue
        f = {}
        for tok in line.split()[1:]:
            if '=' in tok:
                k, _, val = tok.partition('=')
                try:
                    f[k] = float(val)
                except ValueError:
                    pass
        if 'gl_ms' in f:
            gl.append(f['gl_ms'])
            tot.append(f.get('total_ms', 0.0))
            gst.append(f.get('guest_ms', 0.0))
            pres.append(f.get('present_ms', 0.0))
    return gl, tot, gst, pres


def main():
    for spec in sys.argv[1:]:
        label, _, path = spec.partition('=')
        gl, tot, gst, pres = parse(path)
        glw = [v for v in gl if v > 0.0]
        b = stats(gl)
        bw = stats(glw)
        t = stats(tot)
        g = stats(gst)
        p = stats(pres)
        print('%-22s frames=%-6d gl_ms[all]  mean %7.3f med %7.3f p90 %7.3f '
              'sum %9.1f' % (label, len(gl), b[0], b[1], b[2], b[3]))
        print('%-22s         gl_ms[>0]   mean %7.3f med %7.3f p90 %7.3f '
              'n=%d' % ('', bw[0], bw[1], bw[2], len(glw)))
        print('%-22s         total_ms    mean %7.3f med %7.3f p90 %7.3f' %
              ('', t[0], t[1], t[2]))
        print('%-22s         guest_ms    mean %7.3f med %7.3f p90 %7.3f' %
              ('', g[0], g[1], g[2]))
        print('%-22s         present_ms  mean %7.3f med %7.3f p90 %7.3f' %
              ('', p[0], p[1], p[2]))


main()
