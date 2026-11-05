#!/usr/bin/env bash
# tools/fn_emit_pcrel_det.sh -- deterministic 2 M-step PICA A/B for the
# pc-relative / hot-entry roll-out (t_aab205ba).
#
#   BIN=target/dq8-pcrel tools/fn_emit_pcrel_det.sh [tag]
#
# Runs the field savestate twice on the SAME binary -- DQ8_FN_EMIT unset (pure
# dispatcher) and =all (every emitted body enabled) -- and compares the whole
# GPU/PICA command stream plus every DET_* digest.  Byte-identical is the
# whole-program oracle for the emitted bodies: the guest must reach the same
# state and ask the GPU for the same work.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
BIN=${BIN:-target/dq8-pcrel}
TAG=${1:-pcrel}
OUT=${OUT:-target/fn-ev-$TAG}
STATE=${STATE:-/home/hermes/DQ8/evidence/rx580-holes-20261101/scene-holes.sav}
mkdir -p "$OUT"
BASE=(DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_VSPAR=4 DQ8_SPLIT_GL=0
      DQ8_STACK_NO_RELOCATE=1 DQ8_GL_DEPTH_ATTACH=1 DQ8_DETERMINISTIC=1
      DQ8_TRACE_GPU=1 DQ8_BENCH_STEPS=2000000)
for arm in off on; do
  gate=""; [[ $arm == on ]] && gate=all
  /usr/bin/time -f "  $arm wall=%es" env "${BASE[@]}" DQ8_USER_DATA_DIR="$PWD/$OUT/ud-$arm" \
    DQ8_FN_EMIT="$gate" timeout -k 10 1200 "$BIN" \
    --load-state "$STATE" DQ8.code dq8.romfs \
    > "$OUT/det-$arm.out" 2> "$OUT/det-$arm.err"
  grep -E 'GPU|PICA|draw|DRAW|XFER|TEX|SWAP' "$OUT/det-$arm.err" > "$OUT/det-$arm.gpu"
  echo "  $arm: $(wc -l < "$OUT/det-$arm.gpu") lines sha256=$(sha256sum < "$OUT/det-$arm.gpu" | cut -d' ' -f1)"
  grep -h "DET_FRAME\|DET_CLOCK" "$OUT/det-$arm.err" | sed 's/^/    /'
done
if cmp -s "$OUT/det-off.gpu" "$OUT/det-on.gpu"; then
  echo "  PICA TRACE: BYTE-IDENTICAL (gate off vs on, this binary)"
else
  echo "  PICA TRACE: DIFFERENT"; diff "$OUT/det-off.gpu" "$OUT/det-on.gpu" | head -20
fi
