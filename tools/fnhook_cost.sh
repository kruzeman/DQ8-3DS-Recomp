#!/usr/bin/env bash
# tools/fnhook_cost.sh -- hook cost in ns per dispatch entry, all arms in ONE
# binary (t_e1547d93).  DQ8_FN_HOOKMODE selects the arm; the guest runs a fixed
# instruction budget so every arm retires the same 2 M main-context steps and
# the process exits through bench_report() -> atexit -> dq8_fn_report():
#
#   0 no check at all          1 new bitmap+hash check (inline)
#   3 old page-grouped check   4 old flat switch check      5 null noinline call
#   2 bitmap check + dispatch (production; entries < 2 M, bodies retire)
#
# ns/entry is computed by tools/fnhook_cost_sum.py from the FN_HOOK entry count
# and the FRAMEPROF guest_ms sum, so nothing is estimated.
#   usage: [BIN=target/dq8-fnhook] [REPS=3] [STATE=...sav] tools/fnhook_cost.sh
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
BIN=${BIN:-target/dq8-fnhook}
STATE=${STATE:-/home/hermes/DQ8/evidence/hot-regions-20261102/state-battle.sav}
OUT=${OUT:-target/fnhook-cost}
REPS=${REPS:-3}
STEPS=${STEPS:-10000000}
mkdir -p "$OUT"
BASE=(DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_VSPAR=4 DQ8_SPLIT_GL=0
      DQ8_STACK_NO_RELOCATE=1 DQ8_GL_DEPTH_ATTACH=1 DQ8_FRAMEPROF=1
      DQ8_FN_STATS=1 DQ8_BENCH_STEPS=$STEPS)
for rep in $(seq 1 "$REPS"); do
  for mode in 0 1 3 4 5 6 7 8 9 2; do
    gate=""
    [[ $mode == 2 ]] && gate=all
    env "${BASE[@]}" DQ8_FN_HOOKMODE="$mode" DQ8_FN_EMIT="$gate" \
      DQ8_USER_DATA_DIR="$PWD/$OUT/ud" \
      timeout -k 20 900 "$BIN" --load-state "$STATE" DQ8.code dq8.romfs \
      > "$OUT/m$mode-$rep.out" 2> "$OUT/m$mode-$rep.err"
    echo "  rep $rep mode $mode rc=$? $(grep -o 'FN_HOOK mode=[0-9]* entries=[0-9]*' "$OUT/m$mode-$rep.err" | head -1)"
  done
done
python3 tools/fnhook_cost_sum.py "$OUT"
