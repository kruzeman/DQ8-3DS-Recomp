#!/usr/bin/env bash
# tools/fn_bound_det.sh -- deterministic 2 M-step PICA trace, three arms
# (t_21dda9b5).  Acceptance for this card is "byte-identical to dq8-b30":
#   ref  the dq8-b30 binary (feat/fn300 head build) with DQ8_FN_EMIT=all
#   off  this branch's binary, gate off (every function on the dispatcher)
#   on   this branch's binary, exact spill/reload sets, DQ8_FN_EMIT=all
# Compares the GPU/PICA/draw/XFER/TEX line stream and the DET_FRAME digest.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
OUT=${OUT:-target/fn-bound-det}
DATA=${DQ8_EVIDENCE:-/home/hermes/DQ8/evidence}
REF_BIN=${REF_BIN:-/home/hermes/DQ8/worktrees/integ/target/dq8-b30}
BIN=${BIN:-target/dq8-fnbound}
mkdir -p "$OUT"
BASE=(DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_VSPAR=4 DQ8_SPLIT_GL=0
      DQ8_STACK_NO_RELOCATE=1 DQ8_GL_DEPTH_ATTACH=1 DQ8_DETERMINISTIC=1
      DQ8_TRACE_GPU=1 DQ8_BENCH_STEPS=2000000)

run() { # arm bin gate
  local arm=$1 bin=$2 gate=$3
  /usr/bin/time -f "  $arm wall=%es" env "${BASE[@]}" \
    DQ8_USER_DATA_DIR="$PWD/target/ud-det-$arm" DQ8_FN_EMIT="$gate" \
    tools/lock_run.sh --kind measure -- \
    timeout -k 10 900 "$bin" --load-state "$DATA/rx580-holes-20261101/scene-holes.sav" \
    DQ8.code dq8.romfs > "$OUT/$arm.out" 2> "$OUT/$arm.err"
  grep -E 'GPU|PICA|draw|DRAW|XFER|TEX|SWAP' "$OUT/$arm.err" > "$OUT/$arm.gpu"
  printf '  %-3s lines=%s sha256=%s\n' "$arm" "$(wc -l < "$OUT/$arm.gpu")" \
    "$(sha256sum < "$OUT/$arm.gpu" | cut -d' ' -f1)"
  grep -h "DET_FRAME\|DET_CLOCK" "$OUT/$arm.err" | sed 's/^/      /'
  grep -h "^FN_STATS " "$OUT/$arm.err" | sed 's/^/      /'
}

echo "== ref: $REF_BIN (dq8-b30, gate on)"
run ref "$REF_BIN" all
echo "== off: $BIN (gate off)"
run off "$BIN" ""
echo "== on: $BIN (exact sets, gate on)"
run on "$BIN" all

echo "== comparisons"
for pair in "off on" "ref on"; do
  set -- $pair
  if cmp -s "$OUT/$1.gpu" "$OUT/$2.gpu"; then
    echo "  PICA TRACE $1 vs $2: BYTE-IDENTICAL"
  else
    echo "  PICA TRACE $1 vs $2: DIFFERENT"
    diff "$OUT/$1.gpu" "$OUT/$2.gpu" | head -5
  fi
done
