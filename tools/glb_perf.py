#!/usr/bin/env python3
"""t_d559a510: summarise GLPHASE stats from tools/glb_perf.sh runs."""
import glob
import re
import statistics
import sys

PAT = re.compile(r"GLPHASE draws=(\d+) .*draw_ms=([\d.]+) mean_us=(\d+) "
                 r".*blend_gl=(\d+) blend_sw=(\d+) .*glb_sets=(\d+)\s*$")
FRAMES = re.compile(r"TIME window=2\.0s frames=(\d+)")


def main():
    out = sys.argv[1]
    fstat = {}
    for arm in ("gl", "sw"):
        means, dms, draws, sets, frames = [], [], [], [], []
        files = sorted(glob.glob("%s/%s_*" % (out, arm)))
        ratios = []
        for d in files:
            tot = 0
            last = 0
            for ln in open(d + "/run.log", errors="replace"):
                m = PAT.search(ln)
                if m:
                    draws.append(int(m.group(1)))
                    dms.append(float(m.group(2)))
                    means.append(int(m.group(3)))
                    sets.append(int(m.group(6)))
                    tot += int(m.group(1))
                    last = int(m.group(6))
                m = FRAMES.search(ln)
                if m:
                    frames.append(int(m.group(1)))
            if tot:
                ratios.append(float(last) / tot)
        if not means:
            print("%s: NO GLPHASE (%d dirs)" % (arm, len(files)))
            continue
        fstat[arm] = frames
        print("%-3s runs=%d phases=%d  mean_us median=%.0f  draw_ms median=%.1f"
              "  draws/phase median=%.0f  glb_sets median=%.0f  "
              "glb_sets/draw=%.4f"
              % (arm, len(files), len(means), statistics.median(means),
                 statistics.median(dms), statistics.median(draws),
                 statistics.median(sets),
                 statistics.median(ratios) if ratios else 0.0))
        if frames:
            print("    frames/2s: median=%.0f mean=%.1f max=%d n=%d"
                  % (statistics.median(frames), statistics.mean(frames),
                     max(frames), len(frames)))
    if "gl" in fstat and "sw" in fstat and fstat["gl"] and fstat["sw"]:
        g, s = statistics.median(fstat["gl"]), statistics.median(fstat["sw"])
        print("frames/2s median: gl=%.0f sw=%.0f  delta=%+.2f%% (gl vs sw)"
              % (g, s, 100.0 * (g - s) / s))


main()
