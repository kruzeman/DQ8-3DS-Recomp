#!/usr/bin/env bash
# t_05281b89 stage A: deterministic (guest-clock-pinned) run of a savestate at
# one internal render scale, printing the DET_* digests and the coverage lines.
#   tools/rsa_run.sh <scale> <outdir> [steps] [binary] [savestate]
set -u
S=${1:?scale}; OUT=${2:?outdir}; STEPS=${3:-8000000}; BIN=${4:-target/dq8-rsa2}
STATE=${5:-${STATE:-/home/hermes/DQ8/evidence/render-cape-path/dq8-state.sav}}
W=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
UD=${UD:-/home/hermes/.hermes/profiles/renderer/cache/scratch/ud_scale}
EXTRA=${EXTRA:-}
mkdir -p "$OUT" "$UD"
unset DQ8_SAVESTATE_AUTOSAVE_AT
cd "$W"
env DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT" \
    DQ8_RUN_GUEST_THREADS=1 DQ8_USER_DATA_DIR="$UD" DQ8_GL=1 \
    DQ8_RENDER_SCALE="$S" DQ8_DETERMINISTIC=1 \
    DQ8_BENCH_STEPS="$STEPS" DQ8_FRAMEPROF=1 $EXTRA \
    "$BIN" DQ8.code dq8.romfs --load-state "$STATE" > "$OUT/run.log" 2>&1
echo "exit=$?  scale x$S  steps=$STEPS  bin=$BIN  ($OUT)"
grep -m1 "internal render scale" "$OUT/run.log"
grep -m1 "depth self-test:" "$OUT/run.log"
grep -E "^DET_" "$OUT/run.log"
grep -E "render scale coverage|texture read #" "$OUT/run.log" | head -12
md5sum "$OUT"/dq8-screen*.ppm 2>/dev/null
