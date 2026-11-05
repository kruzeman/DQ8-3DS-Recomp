#!/usr/bin/env bash
# One deterministic arm (t_21dda9b5): BIN, gate, tag.
#   usage: tools/fn_bound_one.sh <tag> <binary> <gate> [steps]
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
TAG=${1:?tag}
BIN=${2:?binary}
GATE=${3-}
STEPS=${4:-2000000}
OUT=${OUT:-target/fn-bound-one}
DATA=${DQ8_EVIDENCE:-/home/hermes/DQ8/evidence}
mkdir -p "$OUT"
BASE=(DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_VSPAR=4 DQ8_SPLIT_GL=0
      DQ8_STACK_NO_RELOCATE=1 DQ8_GL_DEPTH_ATTACH=1 DQ8_DETERMINISTIC=1
      DQ8_TRACE_GPU=1 DQ8_BENCH_STEPS="$STEPS")
/usr/bin/time -f "  wall=%es" env "${BASE[@]}" \
  DQ8_USER_DATA_DIR="$PWD/target/ud-one" DQ8_FN_EMIT="$GATE" \
  timeout -k 10 900 "$BIN" --load-state "$DATA/rx580-holes-20261101/scene-holes.sav" \
  DQ8.code dq8.romfs > "$OUT/$TAG.out" 2> "$OUT/$TAG.err"
rc=$?
grep -E 'GPU|PICA|draw|DRAW|XFER|TEX|SWAP' "$OUT/$TAG.err" > "$OUT/$TAG.gpu"
printf '  %-10s rc=%s lines=%s sha=%s\n' "$TAG" "$rc" "$(wc -l < "$OUT/$TAG.gpu")" \
  "$(sha256sum < "$OUT/$TAG.gpu" | cut -d' ' -f1)"
grep -h "DET_CLOCK" "$OUT/$TAG.err" | sed 's/^/      /'
grep -h "^FN_STATS " "$OUT/$TAG.err" | sed 's/^/      /'
grep -h "guest memory fault\|escaped" "$OUT/$TAG.err" | head -2 | sed 's/^/      /'
