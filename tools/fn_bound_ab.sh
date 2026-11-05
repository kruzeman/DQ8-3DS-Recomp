#!/usr/bin/env bash
# tools/fn_bound_ab.sh -- field/battle guest_ms A/B for the FN_EMIT exact-set
# boundary (t_21dda9b5).  Two binaries, interleaved pairs, 25 s per run:
#   BIN_BEFORE  DQ8_FN_EXACT=0 build (dq8-b30's spill-all boundary)
#   BIN_AFTER   exact spill/reload sets
# Both arms run with DQ8_FN_EMIT=all and DQ8_FN_STATS=1 so the same run also
# reports the call-boundary traffic it paid for.
#
#   usage: tools/fn_bound_ab.sh [pairs]
#          BIN_BEFORE=target/dq8-fnbound-before BIN_AFTER=target/dq8-fnbound
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
PAIRS=${1:-3}
BIN_BEFORE=${BIN_BEFORE:-target/dq8-fnbound-before}
BIN_AFTER=${BIN_AFTER:-target/dq8-fnbound}
OUT=${OUT:-target/fn-bound-ab}
DATA=${DQ8_EVIDENCE:-/home/hermes/DQ8/evidence}
mkdir -p "$OUT"
BASE=(DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_VSPAR=4 DQ8_SPLIT_GL=0
      DQ8_STACK_NO_RELOCATE=1 DQ8_GL_DEPTH_ATTACH=1 DQ8_FRAMEPROF=1
      DQ8_FN_EMIT=all DQ8_FN_STATS=1)

declare -A STATES=(
  [field]="$DATA/rx580-holes-20261101/scene-holes.sav"
  [battle]="$DATA/hot-regions-20261102/state-battle.sav"
)

run_arm() { # tag scene arm pair
  local tag=$1 scene=$2 arm=$3 pair=$4
  local bin=$BIN_BEFORE
  [[ $arm == after ]] && bin=$BIN_AFTER
  env "${BASE[@]}" DQ8_USER_DATA_DIR="$PWD/target/ud-$arm" \
    tools/lock_run.sh --kind measure -- \
    timeout -k 5 25 "$bin" --load-state "${STATES[$scene]}" DQ8.code dq8.romfs \
    > "$OUT/$tag-$scene-$pair-$arm.out" 2> "$OUT/$tag-$scene-$pair-$arm.err"
}

summarize() { # file
  awk '
    /^FRAMEPROF f=/ {
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^guest_ms=/) { split($i, x, "="); g = x[2] }
        if ($i ~ /^total_ms=/) { split($i, x, "="); t = x[2] }
      }
      if (t + 0 > 2.0) { s += g; n++; v[n] = g }
    }
    /^FN_STATS / { stats = $0 }
    END {
      if (n) {
        asort(v)
        printf("n=%d mean=%.2f p50=%.2f p90=%.2f ", n, s / n, v[int(n * 0.5) + 1], v[int(n * 0.9) + 1])
      } else printf("n=0 ")
      if (stats != "") { sub(/^FN_STATS /, "", stats); printf(" %s", stats) }
      printf("\n")
    }' "$1"
}

for scene in field battle; do
  echo "########## scene $scene (${STATES[$scene]})"
  for pair in $(seq 1 "$PAIRS"); do
    for arm in before after; do
      run_arm "$scene" "$scene" "$arm" "$pair"
    done
    for arm in before after; do
      printf '  %s %-6s pair %s: ' "$scene" "$arm" "$pair"
      summarize "$OUT/$scene-$pair-$arm.err"
    done
  done
done
