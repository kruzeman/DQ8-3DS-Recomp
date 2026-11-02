#!/usr/bin/env python3
"""t_5ddc492f: summarise paired spec A/B logs (tools/spec_ab.sh).

Per log:
  GLPHASE windows   draws/window, us per draw inside glDrawArrays (sub_ms/draw),
                    mean box px, how many draws were served by a generated
                    program (spec=) vs the generic fallback (gen=), nvar
  FRAMEPROF frames  frame mean, % > 50 ms, mean of the heavy frames, worst
  GLSHAPE / GLSPEC  aggregated histograms when the knobs were on
"""
import glob
import os
import re
import sys

GL = re.compile(r"GLPHASE draws=(\d+) .*?sub_ms=([\d.]+) .*?boxpx=(\d+) "
                r".*?spec=(\d+) gen=(\d+) schfail=(\d+) nvar=(\d+)")
FR = re.compile(r"FRAMEPROF f=(\d+) t=[\d.]+ total_ms=([\d.]+)")
SHAPE = re.compile(r"GLSHAPE (\S+) draws=(\d+) px=(\d+) ms=([\d.]+)")
VAR = re.compile(r"GLSPEC var=(\d+) key=([0-9a-f]+) draws=(\d+) px_per=(\d+) "
                 r"sub_ms=([\d.]+) us_per_draw=(\d+)")
# DRAWPHASE: the deferred write-back (llvmpipe rasterisation surfaces here)
DP = re.compile(r"DRAWPHASE n=(\d+) .*?flush_ms=([\d.]+) .*?mean_us=(\d+)")


def summarise(path):
    draws, sub, boxpx, spec, gen, nvar = [], [], [], 0, 0, 0
    flush = []
    shapes, vars_ = {}, {}
    frames = []
    first = None
    for line in open(path, errors="replace"):
        m = GL.search(line)
        if m:
            d = int(m.group(1))
            s = float(m.group(2))
            if first is None:
                first = (d, s, int(m.group(4)), int(m.group(5)), int(m.group(7)))
            draws.append(d)
            sub.append(s)
            boxpx.append(int(m.group(3)))
            spec += int(m.group(4))
            gen += int(m.group(5))
            nvar = max(nvar, int(m.group(7)))
            continue
        m = DP.search(line)
        if m:
            flush.append((int(m.group(1)), float(m.group(2))))
            continue
        m = FR.search(line)
        if m:
            frames.append(float(m.group(2)))
            continue
        m = SHAPE.search(line)
        if m:
            k = m.group(1)
            shapes[k] = (shapes.get(k, 0) + int(m.group(2)),
                         shapes.get(k, (0, 0))[1] + float(m.group(4)))
            continue
        m = VAR.search(line)
        if m:
            k = m.group(2)
            v = vars_.get(k, [0, 0.0, 0])
            vars_[k] = [v[0] + int(m.group(3)), v[1] + float(m.group(5)),
                        int(m.group(6))]
    out = {"name": os.path.basename(path)[:-4], "frames": frames,
           "nwin": len(draws), "nvar": nvar, "spec": spec, "gen": gen,
           "first": first, "shapes": shapes, "vars": vars_}
    if flush:
        f = flush[1:] or flush
        out["flush"] = sum(x[1] for x in f) / len(f)
        out["flush_draws"] = sum(x[0] for x in f) / len(f)
    if draws:
        # window 1 is the warm-up (savestate load + first-use shader JIT)
        use = slice(1, None) if len(draws) > 3 else slice(0, None)
        d = draws[use] or draws
        s = sub[use] or sub
        b = boxpx[use] or boxpx
        out["draws"] = sum(d) / len(d)
        out["us"] = 1000.0 * sum(s) / max(1, sum(d))
        out["boxpx"] = sum(b) / len(b)
        out["windows"] = len(d)
    if frames:
        fr = frames[3:] or frames
        heavy = [x for x in fr if x > 50.0]
        out["fmean"] = sum(fr) / len(fr)
        out["pct50"] = 100.0 * len(heavy) / len(fr)
        out["heavym"] = sum(heavy) / len(heavy) if heavy else 0.0
        out["worst"] = max(fr)
        out["nframes"] = len(fr)
    return out


def line(o):
    s = ("%-10s wraps=%2d draws=%6.1f us/draw=%7.1f boxpx=%6.0f " %
         (o["name"], o["windows"], o["draws"], o["us"], o["boxpx"]))
    s += ("frames=%3d mean=%6.2f >50ms=%5.1f%% heavy_mean=%6.1f worst=%6.1f" %
          (o["nframes"], o["fmean"], o["pct50"], o["heavym"], o["worst"]))
    if "flush" in o:
        s += " flush_ms/win=%6.1f (%.0f us/draw)" % (
            o["flush"], 1000.0 * o["flush"] / o["flush_draws"])
    return s


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    paths = sorted(glob.glob(os.path.join(d, "p*_spec[01].log")))
    if not paths:
        print("no logs in", d)
        return 1
    res = [summarise(p) for p in paths]
    for o in res:
        if "draws" in o:
            print(line(o))
        if o["first"]:
            f = o["first"]
            print("           first window: draws=%d sub_ms=%.1f spec=%d gen=%d "
                  "nvar=%d" % f)
    # paired deltas
    pairs = {}
    for o in res:
        m = re.match(r"p(\d+)_spec(\d)", o["name"])
        if m:
            pairs.setdefault(int(m.group(1)), {})[int(m.group(2))] = o
    for p, v in sorted(pairs.items()):
        if 0 not in v or 1 not in v:
            continue
        a, b = v[0], v[1]
        if "us" not in a or "us" not in b:
            continue
        print("pair %d: us/draw %.1f -> %.1f (%+.1f%%), frame mean %.2f -> "
              "%.2f (%+.1f%%), draws/win %.1f -> %.1f (%+.1f%%)" %
              (p, a["us"], b["us"], 100.0 * (b["us"] - a["us"]) / a["us"],
               a["fmean"], b["fmean"], 100.0 * (b["fmean"] - a["fmean"]) / a["fmean"],
               a["draws"], b["draws"], 100.0 * (b["draws"] - a["draws"]) / a["draws"]))
    # histograms (spec0 side is the interesting baseline)
    for o in res:
        if o["shapes"]:
            print("\n-- shapes (%s) --" % o["name"])
            for k, (n, ms) in sorted(o["shapes"].items(), key=lambda x: -x[1][1])[:14]:
                print("  %-52s draws=%5d ms=%8.1f" % (k, n, ms))
        if o["vars"]:
            print("-- variants (%s) --" % o["name"])
            for k, (n, ms, us) in sorted(o["vars"].items(), key=lambda x: -x[1][1]):
                print("  key=%s draws=%5d ms=%8.1f us/draw=%6d" % (k, n, ms, us))
    return 0


if __name__ == "__main__":
    sys.exit(main())
