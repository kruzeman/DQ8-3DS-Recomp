#!/usr/bin/env bash
# tools/fn_emit_ind_ab.sh -- field/battle guest-ms A/B for the indirect-exit
# roll-out (t_ba4d43cb).  Indicative only: this host has no GPU (llvmpipe), so
# wall time is dominated by rendering; the signal is DQ8_FRAMEPROF's guest_ms
# per frame, the pilot's measure.
#   usage: BIN=target/dq8-fnind STATE=... tools/fn_emit_ind_ab.sh <tag> <state> [pairs]
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
BIN=${BIN:-target/dq8-fnind}
TAG=${1:?tag}
STATE=${2:?savestate}
PAIRS=${3:-3}
OUT=${OUT:-target/fn-ev-ab}
mkdir -p "$OUT"
BASE=(DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_VSPAR=4 DQ8_SPLIT_GL=0
      DQ8_STACK_NO_RELOCATE=1 DQ8_GL_DEPTH_ATTACH=1 DQ8_FRAMEPROF=1)
for pair in $(seq 1 "$PAIRS"); do
  for arm in off on; do
    gate=""; [[ $arm == on ]] && gate=all
    env "${BASE[@]}" DQ8_USER_DATA_DIR=$PWD/target/ud-$arm DQ8_FN_EMIT="$gate" \
      timeout -k 5 25 "$BIN" --load-state "$STATE" DQ8.code dq8.romfs \
      > "$OUT/$TAG-$pair-$arm.out" 2> "$OUT/$TAG-$pair-$arm.err"
  done
  for arm in off on; do
    awk -v a="$arm" '/^FRAMEPROF f=/{ for(i=1;i<=NF;i++){ if($i ~ /^guest_ms=/){split($i,x,"=");g=x[2]} if($i ~ /^total_ms=/){split($i,x,"=");t=x[2]} } if (t+0 > 2.0) {s+=g;n++} } END {printf "  %s %s pair %s: guest_ms mean=%.3f frames=%d\n", "'"$TAG"'", a, "'"$pair"'", (n? s/n:0), n}' "$OUT/$TAG-$pair-$arm.err"
  done
done
