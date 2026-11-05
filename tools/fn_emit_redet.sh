#!/usr/bin/env bash
# Re-verify the deterministic 2M-step PICA trace A/B on the FINAL binary.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
OUT=${OUT:-target/fn-ev-redet}
DATA=${DQ8_EVIDENCE:-/home/hermes/DQ8/evidence}
mkdir -p "$OUT"
BASE=(DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_VSPAR=4 DQ8_SPLIT_GL=0
      DQ8_STACK_NO_RELOCATE=1 DQ8_GL_DEPTH_ATTACH=1 DQ8_DETERMINISTIC=1
      DQ8_TRACE_GPU=1 DQ8_BENCH_STEPS=2000000)
for arm in off on; do
  gate=""; [[ $arm == on ]] && gate=all
  /usr/bin/time -f "  $arm wall=%es" env "${BASE[@]}" DQ8_USER_DATA_DIR=$PWD/target/ud-$arm \
    DQ8_FN_EMIT="$gate" timeout -k 10 600 ./target/dq8-fnind \
    --load-state "$DATA/rx580-holes-20261101/scene-holes.sav" DQ8.code dq8.romfs \
    > "$OUT/det-$arm.out" 2> "$OUT/det-$arm.err"
  # Everything the guest asks the GPU/display to do.  BENCH/BENCH_THREAD
  # accounting lines are deliberately excluded: where the 2 M step limit lands
  # differs by a few instructions, and that is the only difference.
  grep -E 'GPU|PICA|draw|DRAW|XFER|TEX|SWAP' "$OUT/det-$arm.err" > "$OUT/det-$arm.gpu"
  echo "  $arm: $(wc -l < "$OUT/det-$arm.gpu") lines sha256=$(sha256sum < "$OUT/det-$arm.gpu" | cut -d' ' -f1)"
  grep -h "DET_FRAME\|DET_CLOCK" "$OUT/det-$arm.err" | sed 's/^/    /'
done
if cmp -s "$OUT/det-off.gpu" "$OUT/det-on.gpu"; then
  echo "  PICA TRACE: BYTE-IDENTICAL (gate off vs on, this binary)"
else
  echo "  PICA TRACE: DIFFERENT"; diff "$OUT/det-off.gpu" "$OUT/det-on.gpu" | head -5
fi
