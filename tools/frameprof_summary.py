#!/usr/bin/env python3
"""Summarise FRAMEPROF lines (DQ8_FRAMEPROF=1) from one or more logs.

usage: frameprof_summary.py <name>=<log> [...]
Prints frames, wall span, fps, and mean/p50/p90 total and guest ms over the
frames that actually rendered (total_ms > 5 ms), which is the set the frame
budget reports use.
"""
import re
import sys

RE = re.compile(r"FRAMEPROF f=(\d+) t=([\d.]+) total_ms=([\d.]+) guest_ms=([\d.]+)"
                r" gl_ms=([\d.]+) present_ms=([\d.]+) audio_ms=([\d.]+)"
                r" idle_ms=([\d.]+) other_ms=([\d.]+)")


def main():
    print("| run | frames | rendered | fps | mean total | p50 | mean guest | p90 guest | guest s |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for spec in sys.argv[1:]:
        name, path = spec.split("=", 1)
        frames = []
        for line in open(path, errors="replace"):
            m = RE.search(line)
            if m:
                g = [float(x) for x in m.groups()]
                frames.append((g[1], g[2], g[3]))
        if not frames:
            print("| %s | 0 | - | - | - | - | - | - | - |" % name)
            continue
        rendered = [f for f in frames if f[1] > 5.0] or frames
        totals = sorted(f[1] for f in rendered)
        guests = sorted(f[2] for f in rendered)
        span = frames[-1][0] - frames[0][0]
        n = len(rendered)
        print("| %s | %d | %d | %.1f | %.2f | %.2f | %.2f | %.2f | %.2f |" % (
            name, len(frames), n, n / span if span > 0 else 0.0,
            sum(totals) / n, totals[n // 2], sum(guests) / n,
            guests[int(n * 0.9)], sum(guests) / 1000.0))


if __name__ == "__main__":
    main()
