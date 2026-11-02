#!/usr/bin/env bash
# t_03f3f620: single-core-pinned guest-throughput A/B.  Both arms are pinned to
# the same CPU, so the number is the pure dispatch cost per retired guest
# instruction on ONE core (the metric the operator's "1 core at 95%" is about).
# usage: tools/ab_frozen_pinned.sh <cpu> <pairs> <steps> <outfile.jsonl>
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
CPU=${1:-2}
PAIRS=${2:-6}
STEPS=${3:-10000000}
OUTJ=${4:-evidence/t_03f3f620/tp_pinned.jsonl}
S0=/home/hermes/DQ8/worktrees/ab-s0-frozen/target/dq8-ab-s0
S1=/home/hermes/DQ8/worktrees/ab-s1-frozen/target/dq8-ab-s1
mkdir -p "$(dirname "$OUTJ")"
: > "$OUTJ"
for i in $(seq 1 "$PAIRS"); do
  if (( i % 2 == 1 )); then first=$S0; second=$S1; else first=$S1; second=$S0; fi
  for bin in "$first" "$second"; do
    taskset -c "$CPU" python3 tools/ab_cores.py "pin-$CPU-$(basename "$bin")-$i" \
      "$bin" 0 --steps "$STEPS" >> "$OUTJ"
  done
done
python3 tools/ab_frozen_summary.py "$OUTJ"
