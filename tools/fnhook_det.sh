#!/usr/bin/env bash
# tools/fnhook_det.sh -- deterministic 2 M-step oracle for the entry-check card
# (t_e1547d93).  Three arms on the same field savestate:
#
#   off   target/dq8-fnhook, DQ8_FN_EMIT unset   (pure dispatcher, new hook present)
#   on    target/dq8-fnhook, DQ8_FN_EMIT=all     (all 2 800 bodies)
#   b41   dq8-b41, no FN_EMIT in the binary      (feat/cull-shadow build = the base)
#
# Acceptance 4: the deterministic PICA trace and every DET_* digest must equal
# dq8-b41 for the arm with the same function list -- b41 has no emitted
# functions, so that arm is `off`; `on` must in turn equal `off` (the
# whole-program oracle for the emitted bodies).
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
BIN=${BIN:-target/dq8-fnhook}
B41=${B41:-/home/hermes/DQ8/worktrees/render-t_e216fc37/target/dq8-b41}
STATE=${STATE:-/home/hermes/DQ8/evidence/rx580-holes-20261101/scene-holes.sav}
OUT=${OUT:-target/fnhook-det}
mkdir -p "$OUT"
BASE=(DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_VSPAR=4 DQ8_SPLIT_GL=0
      DQ8_STACK_NO_RELOCATE=1 DQ8_GL_DEPTH_ATTACH=1 DQ8_DETERMINISTIC=1
      DQ8_TRACE_GPU=1 DQ8_BENCH_STEPS=2000000)
run() { # arm bin gate
  local arm=$1 bin=$2 gate=$3
  /usr/bin/time -f "  $arm wall=%es" env "${BASE[@]}" DQ8_FN_EMIT="$gate" \
    DQ8_USER_DATA_DIR="$PWD/$OUT/ud-$arm" \
    timeout -k 10 1800 "$bin" --load-state "$STATE" DQ8.code dq8.romfs \
    > "$OUT/det-$arm.out" 2> "$OUT/det-$arm.err"
  echo "  $arm exit=$?"
  grep -E 'GPU|PICA|draw|DRAW|XFER|TEX|SWAP' "$OUT/det-$arm.err" > "$OUT/det-$arm.gpu"
  echo "  $arm: $(wc -l < "$OUT/det-$arm.gpu") PICA/GPU lines sha256=$(sha256sum < "$OUT/det-$arm.gpu" | cut -d' ' -f1)"
  grep -h "DET_" "$OUT/det-$arm.err" | sed 's/^/    /'
}
run off "$BIN" ""
run on  "$BIN" all
run b41 "$B41" ""
echo
for pair in "off on" "off b41"; do
  set -- $pair
  if cmp -s "$OUT/det-$1.gpu" "$OUT/det-$2.gpu"; then
    echo "  PICA TRACE $1 vs $2: BYTE-IDENTICAL"
  else
    echo "  PICA TRACE $1 vs $2: DIFFERENT"; diff "$OUT/det-$1.gpu" "$OUT/det-$2.gpu" | head -10
  fi
done
echo "  DET digest diff (off vs b41, and off vs on):"
for a in b41 on; do
  diff <(grep "^DET_" "$OUT/det-off.err" | grep -v DET_CLOCK) \
       <(grep "^DET_" "$OUT/det-$a.err" | grep -v DET_CLOCK) > "$OUT/det-digest-$a.diff" \
    && echo "    off vs $a: DET_* identical (DET_CLOCK excluded: wall-clock dependent)" \
    || { echo "    off vs $a: DIFFERENT"; head -8 "$OUT/det-digest-$a.diff"; }
done
