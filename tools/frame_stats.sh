#!/usr/bin/env bash
# frame_stats.sh A B — per-frame diff% summary of top screens.
set -u
cd /home/hermes/DQ8
A=$1; B=$2
for f in "$A"/t*.ppm; do
  n=$(basename "$f")
  case "$n" in *bottom*) continue ;; esac
  [ -f "$B/$n" ] || continue
  pct=$(python3 tools/compare_frames.py "$f" "$B/$n" | sed -n 's/.*(\([0-9.]*\)% of bytes.*/\1/p')
  echo "$n $pct"
done | sort -k2 -g | awk '
  {v[NR]=$2; s+=$2; if ($2>m) {m=$2; mx=$1}}
  END {printf "n=%d mean=%.3f%% max=%.3f%% (%s) median=%.3f%%\n",
       NR, s/NR, m, mx, v[int((NR+1)/2)]}'
