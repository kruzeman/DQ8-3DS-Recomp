#!/usr/bin/env bash
# t_03f3f620: interleaved frozen-snapshot A/B driver.
# usage: tools/ab_frozen_run.sh <mode tp|fps> <pairs> <dur_s> <outfile.jsonl>
#   tp  : fixed 10M-step guest budget (throughput + CPU per fixed work)
#   fps : duration-based savestate scene run (fps + per-core utilisation)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
MODE=${1:?mode tp|fps}
PAIRS=${2:-6}
DUR=${3:-60}
OUTJ=${4:-evidence/t_03f3f620/$MODE.jsonl}
S0=/home/hermes/DQ8/worktrees/ab-s0-frozen/target/dq8-ab-s0
S1=/home/hermes/DQ8/worktrees/ab-s1-frozen/target/dq8-ab-s1
mkdir -p "$(dirname "$OUTJ")"
: > "$OUTJ"
for i in $(seq 1 "$PAIRS"); do
  if (( i % 2 == 1 )); then first=$S0; second=$S1; else first=$S1; second=$S0; fi
  for bin in "$first" "$second"; do
    tag=$(basename "$bin")
    if [[ $MODE == tp ]]; then
      python3 tools/ab_cores.py "tp-$tag-$i" "$bin" 0 --steps ${STEPS:-10000000} >> "$OUTJ"
    else
      python3 tools/ab_cores.py "fps-$tag-$i" "$bin" "$DUR" >> "$OUTJ"
    fi
  done
done
python3 tools/ab_frozen_summary.py "$OUTJ"
