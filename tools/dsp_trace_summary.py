#!/usr/bin/env python3
"""Summarise the 'dsp audio: wall=' trace lines of an audio probe run."""
import re
import statistics
import sys

path = sys.argv[1]
fields = ("wall", "sink_frames", "sink_rate", "pacer_frames", "resyncs", "drops",
          "deadline_misses", "sink_empty", "content_gaps", "sink_drops",
          "commits", "prefill", "lag_us")
rows = []
pat = re.compile(r"dsp audio: wall=([\d.]+) sink_frames=(\d+) sink_rate=([\d.]+)/s "
                 r"pacer_frames=(\d+) resyncs=(\d+) drops=(\d+) deadline_misses=(\d+) "
                 r"sink_empty=(\d+) content_gaps=(\d+) sink_drops=(\d+) commits=(\d+) "
                 r"prefill=(\d+) lag_us=(-?[\d.]+)")
for line in open(path, errors="replace"):
    m = pat.search(line)
    if m:
        rows.append(dict(zip(fields, [float(x) for x in m.groups()])))
if not rows:
    print("no trace lines")
    sys.exit(1)
rates = [r["sink_rate"] for r in rows]
lags = [r["lag_us"] / 1000.0 for r in rows]
total_wall = sum(r["wall"] for r in rows)
frames = rows[-1]["sink_frames"] - rows[0]["sink_frames"]
print("lines=%d wall=%.1fs sink_frames=%d overall_sink_rate=%.2f/s (nominal 204.55/s)"
      % (len(rows), total_wall, frames, frames / total_wall if total_wall else 0))
print("per-second sink_rate: min=%.1f median=%.1f mean=%.1f max=%.1f under_100=%d/%d"
      % (min(rates), statistics.median(rates), statistics.mean(rates), max(rates),
         sum(1 for r in rates if r < 100), len(rates)))
print("lag_ms: min=%.1f median=%.1f max=%.1f" % (min(lags), statistics.median(lags), max(lags)))
for key in ("resyncs", "drops", "deadline_misses", "sink_empty", "content_gaps",
            "sink_drops", "commits", "prefill"):
    print("%s: first=%d last=%d delta=%d" % (key, rows[0][key], rows[-1][key],
                                             rows[-1][key] - rows[0][key]))
worst = sorted(rows, key=lambda r: r["sink_rate"])[:5]
print("slowest seconds:", ", ".join("%.1f/s" % r["sink_rate"] for r in worst))
