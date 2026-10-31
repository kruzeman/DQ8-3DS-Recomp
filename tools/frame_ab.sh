#!/usr/bin/env bash
# frame_ab.sh — compare top-screen tNNNN frames of two evidence dirs.
set -u
cd /home/hermes/DQ8
A=$1; B=$2
same=0; diffn=0; firstdiff=""
for f in "$A"/t*.ppm; do
  n=$(basename "$f")
  case "$n" in *bottom*) continue ;; esac
  [ -f "$B/$n" ] || continue
  if python3 tools/compare_frames.py "$f" "$B/$n" | grep -q IDENTICAL; then
    same=$((same+1))
  else
    diffn=$((diffn+1))
    if [ -z "$firstdiff" ]; then
      firstdiff="$n $(python3 tools/compare_frames.py "$f" "$B/$n" | grep VERDICT)"
    fi
  fi
done
echo "identical=$same different=$diffn"
echo "first_diff: $firstdiff"
