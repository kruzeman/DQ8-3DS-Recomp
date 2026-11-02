#!/usr/bin/env python3
"""t_8df70ad4: summarize FRAMEPROF runs + PGLFLUSH windows + PPM identity.

usage: zz_an.py <run-dir> [<run-dir> ...]
"""
import sys, os, re, hashlib

def ppm_stats(path):
    try:
        d = open(path, 'rb').read()
    except OSError:
        return None
    if not d.startswith(b'P6'):
        return None
    i, toks = 2, []
    while len(toks) < 3:
        while d[i:i+1].isspace():
            i += 1
        if d[i:i+1] == b'#':
            while d[i:i+1] != b'\n':
                i += 1
            continue
        j = i
        while not d[j:j+1].isspace():
            j += 1
        toks.append(int(d[i:j])); i = j
    i += 1
    w, h, _ = toks
    px = d[i:]
    n = w * h
    if len(px) < n * 3:
        return ('short', w, h)
    dark = 0
    s = 0
    step = max(1, n // 20000)
    cnt = 0
    for k in range(0, n, step):
        o = k * 3
        r, g, b = px[o], px[o+1], px[o+2]
        if max(r, g, b) < 24:
            dark += 1
        s += (r + g + b) / 3.0
        cnt += 1
    return (w, h, 100.0 * dark / cnt, s / cnt,
            hashlib.sha256(px).hexdigest()[:16])

def main():
    for d in sys.argv[1:]:
        out = os.path.join(d, 'out.log')
        err = os.path.join(d, 'err.log')
        tot = []
        gs = []
        gl = []
        last_t = 0.0
        for src in (out, err):
            if not os.path.exists(src):
                continue
            for ln in open(src, errors='replace'):
                m = re.match(r'FRAMEPROF f=(\d+) t=([\d.]+) total_ms=([\d.]+) '
                             r'guest_ms=([\d.]+) gl_ms=([\d.]+)', ln)
                if m:
                    t = float(m.group(2)); tot.append(float(m.group(3)))
                    gs.append(float(m.group(4))); gl.append(float(m.group(5)))
                    last_t = max(last_t, t)
        rendered = [x for x in tot if x > 5.0]
        rendered.sort()
        p50 = rendered[len(rendered)//2] if rendered else 0.0
        p90 = rendered[int(len(rendered)*0.9)] if rendered else 0.0
        fps = len(rendered) / last_t if last_t else 0.0
        mg = sum(gs) / len(gs) if gs else 0.0
        mgl = sum(gl) / len(gl) if gl else 0.0
        print('%-18s frames=%-5d rendered=%-4d fps=%5.1f p50=%6.2f p90=%6.2f '
              'guest_mean=%6.2f gl_mean=%5.2f' %
              (os.path.basename(d.rstrip('/')), len(tot), len(rendered), fps,
               p50, p90, mg, mgl))
        if os.path.exists(err):
            pgl = [ln.strip() for ln in open(err, errors='replace')
                   if ln.startswith('PGLFLUSH')]
            if pgl:
                print('   PGLFLUSH lines=%d' % len(pgl))
                for ln in pgl[:3] + (['   ...'] if len(pgl) > 4 else []) + pgl[-1:]:
                    print('   ' + ln)
            for pat in ('escaped', 'guest memory fault'):
                hits = [ln.strip() for ln in open(err, errors='replace')
                        if pat in ln]
                if hits:
                    print('   !!! %s x%d: %s' % (pat, len(hits), hits[0]))
        for name in ('dq8-screen.ppm', 'dq8-screen-bottom.ppm'):
            p = os.path.join(d, name)
            st = ppm_stats(p)
            if st:
                print('   %-20s %dx%d dark=%.1f%% mean=%.1f sha=%s' %
                      ((name,) + st))

if __name__ == '__main__':
    main()
