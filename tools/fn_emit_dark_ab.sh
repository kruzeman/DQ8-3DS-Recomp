#!/usr/bin/env bash
# tools/fn_emit_dark_ab.sh -- capture-level A/B for the FN_EMIT roll-out
# (t_ba4d43cb): mean per-frame DARKLOG darkness of the presented frame with the
# gate off and on, for the three scenes the card names (town map / field /
# battle).  The historic "1.5 / 37.7 / 59.0 % dark" triple does not reproduce on
# this build lineage (see reports/DQ8_ENV_GATE_CACHE_t_275eb521.md), so the check
# is off-vs-on identity on the same scenes, not the absolute numbers.
#   usage: tools/fn_emit_dark_ab.sh [seconds]
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
BIN=${BIN:-target/dq8-fnind}
SECS=${1:-12}
OUT=${OUT:-target/fn-ev-dark}
DATA=${DQ8_EVIDENCE:-/home/hermes/DQ8/evidence}
mkdir -p "$OUT"
BASE=(DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_VSPAR=4 DQ8_SPLIT_GL=0
      DQ8_STACK_NO_RELOCATE=1 DQ8_GL_DEPTH_ATTACH=1 DQ8_DARKLOG=1)
run() { # tag state arm
  local tag=$1 state=$2 arm=$3 gate=""
  [[ $arm == on ]] && gate=all
  env "${BASE[@]}" DQ8_USER_DATA_DIR=$PWD/target/ud-$arm DQ8_FN_EMIT="$gate" \
    timeout -k 5 "$SECS" "$BIN" --load-state "$state" DQ8.code dq8.romfs \
    > "$OUT/$tag-$arm.out" 2> "$OUT/$tag-$arm.err"
  awk -v t="$tag" -v a="$arm" '
    /^DARKLOG / { for (i=1;i<=NF;i++) {
        if ($i ~ /^shadow=/) { split($i,x,"="); s+=x[2]; n++ }
        if ($i ~ /^rt_dark=/) { split($i,x,"="); r+=x[2]; m++ } } }
    END { printf "  %-8s %-3s frames=%4d shadow_dark=%5.1f%% rt_dark=%5.1f%%\n",
                 t, a, n, (n?s/n:0), (m?r/m:0) }' "$OUT/$tag-$arm.err"
}
for spec in "map:$DATA/map-black-20261101/map.sav" \
            "field:$DATA/rx580-holes-20261101/scene-holes.sav" \
            "battle:$DATA/hot-regions-20261102/state-battle.sav"; do
  tag=${spec%%:*}; state=${spec#*:}
  for arm in off on; do run "$tag" "$state" "$arm"; done
done
