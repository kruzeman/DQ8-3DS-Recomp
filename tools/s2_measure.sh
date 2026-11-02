#!/usr/bin/env bash
# t_6edf5d17: one stage's measured A/B (fps scene + fixed-work guest
# throughput), interleaved, plus the summary tables.
# usage: tools/s2_measure.sh <tag> <baseline-bin> <stage-bin> [fps_pairs] [tp_pairs]
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TAG=${1:?tag}
A=${2:?baseline}
B=${3:?stage}
FP=${4:-3}
TP=${5:-6}
EV=evidence/t_6edf5d17
for pair in "fps $FP 70" "tp $TP 10000000"; do
  set -- $pair
  mode=$1; n=$2; val=$3
  out="$EV/ab-$TAG-$mode.jsonl"
  echo "== $TAG $mode: $n interleaved pairs ($A vs $B) $(date -Is)"
  bash tools/s2_ab.sh "$mode" "$n" "$val" "$A" "$B" "$out"
  python3 tools/s2_summary.py "$out" | tee "$EV/ab-$TAG-$mode.summary.txt"
done
echo "== $TAG measurement done $(date -Is)"
