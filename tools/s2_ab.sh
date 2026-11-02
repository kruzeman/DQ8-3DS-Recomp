#!/usr/bin/env bash
# t_6edf5d17: interleaved frozen-binary A/B driver for the S2 stages.
# usage: tools/s2_ab.sh <tp|fps> <pairs> <dur_s|steps> <binA> <binB> <out.jsonl>
#   A = baseline arm (label ...-A-<i>), B = stage arm (label ...-B-<i>).
#   order alternates per pair: A,B then B,A ...
#   mode tp : fixed DQ8_BENCH_STEPS guest budget (dur_s arg = steps)
#   mode fps: duration-based savestate scene run
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
MODE=${1:?tp|fps}
PAIRS=${2:-3}
VAL=${3:-70}
BINA=${4:?baseline binary}
BINB=${5:?stage binary}
OUTJ=${6:?out jsonl}
OUTD=$(dirname "$OUTJ")
mkdir -p "$OUTD"
: > "$OUTJ"
for i in $(seq 1 "$PAIRS"); do
  if (( i % 2 == 1 )); then first=$BINA; firstarm=A; second=$BINB; secondarm=B
  else first=$BINB; firstarm=B; second=$BINA; secondarm=A; fi
  for arm in "$firstarm:$first" "$secondarm:$second"; do
    a=${arm%%:*}; bin=${arm#*:}
    if [[ $MODE == tp ]]; then
      python3 tools/ab_cores.py "tp-$a-$i" "$bin" 0 --steps "$VAL" --out "$OUTD" >> "$OUTJ"
    else
      python3 tools/ab_cores.py "fps-$a-$i" "$bin" "$VAL" --out "$OUTD" >> "$OUTJ"
    fi
  done
done
echo "== $MODE A/B done: $OUTJ"
