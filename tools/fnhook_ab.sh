#!/usr/bin/env bash
# tools/fnhook_ab.sh -- interleaved guest_ms A/B for the FN_EMIT entry-check
# card (t_e1547d93).  Same host, same scene, arms interleaved per repetition:
#
#   b35       dq8-b35             300 functions, old flat hook (the card's baseline)
#   new2800   target/dq8-fnhook   2 800 functions, new bitmap+hash hook
#   new300    target/dq8-fnhook   the 300-function subset of the same binary
#   off       target/dq8-fnhook   no emitted function (pure dispatcher, new hook present)
#   b41       dq8-b41             no FN_EMIT in the binary at all (hook-free reference)
#
# guest_ms comes from FRAMEPROF and is the PF_GUEST wall-clock zone per frame;
# runs are 25 s and frames with total_ms <= 2.0 (startup) are dropped.
#   usage: tools/fnhook_ab.sh [secs] [reps]
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
SECS=${SECS:-25}
REPS=${REPS:-3}
OUT=${OUT:-target/fnhook-ab}
NEW=${NEW:-target/dq8-fnhook}
B35=${B35:-/home/hermes/DQ8/worktrees/integ/target/dq8-b35}
B41=${B41:-/home/hermes/DQ8/worktrees/render-t_e216fc37/target/dq8-b41}
FIELD=${FIELD:-/home/hermes/DQ8/evidence/hot-regions-20261102/state-field.sav}
BATTLE=${BATTLE:-/home/hermes/DQ8/evidence/hot-regions-20261102/state-battle.sav}
L300=$(python3 tools/_list_env.py tools/fn_emit_top300.txt)
mkdir -p "$OUT"
BASE=(DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_VSPAR=4 DQ8_SPLIT_GL=0
      DQ8_STACK_NO_RELOCATE=1 DQ8_GL_DEPTH_ATTACH=1 DQ8_FRAMEPROF=1)
: > "$OUT/runs.tsv"
run() { # scene bin gate tag
  local scene=$1 bin=$2 gate=$3 tag=$4
  env "${BASE[@]}" DQ8_FN_EMIT="$gate" DQ8_USER_DATA_DIR="$PWD/$OUT/ud-$tag" \
    timeout -k 10 "$SECS" "$bin" --load-state "$SCENE" DQ8.code dq8.romfs \
    > "$OUT/$scene-$tag-$rep.out" 2> "$OUT/$scene-$tag-$rep.err"
  local rc=$?
  local mean
  mean=$(awk '/^FRAMEPROF f=/{g="";t="";for(i=1;i<=NF;i++){if($i~/^guest_ms=/){split($i,x,"=");g=x[2]}if($i~/^total_ms=/){split($i,x,"=");t=x[2]}}if(t+0>2.0){s+=g;n++}}END{if(n)printf "%.3f",s/n;else printf "nan"}' "$OUT/$scene-$tag-$rep.err")
  local frames
  frames=$(grep -c '^FRAMEPROF f=' "$OUT/$scene-$tag-$rep.err")
  printf "%s\t%s\t%s\t%s\t%s\t%s\n" "$scene" "$tag" "$rep" "$mean" "$frames" "$rc" >> "$OUT/runs.tsv"
  echo "  $scene $tag rep$rep guest_ms=$mean frames=$frames rc=$rc"
}
for scene in field battle; do
  if [[ $scene == field ]]; then SCENE=$FIELD; else SCENE=$BATTLE; fi
  for rep in $(seq 1 "$REPS"); do
    run "$scene" "$B35" all b35
    run "$scene" "$B35" "" b35off
    run "$scene" "$NEW" all new2800
    run "$scene" "$NEW" "$L300" new300
    run "$scene" "$NEW" "" off
    run "$scene" "$B41" "" b41
  done
done
python3 tools/fnhook_ab_sum.py "$OUT/runs.tsv"
