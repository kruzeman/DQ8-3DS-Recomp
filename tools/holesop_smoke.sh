#!/usr/bin/env bash
# t_a22413f1: quick GL/SW smoke on the operator savestate (2D load menu) —
# catches integration breakage of the new hardware depth attachment without a
# 9-minute scene run.
set -u
W=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${OUT:-$W/evidence/smoke-t_a22413f1}
SECS=${SECS:-25}
BIN=${BIN:-$W/target/dq8-holesop}
STATE=${STATE:-/home/hermes/DQ8/saves/dq8-state.sav}
mkdir -p "$OUT"
cd "$W"
run() {
  local gl=$1 d="$OUT/gl$1"
  mkdir -p "$d"; rm -f "$d"/dq8-screen*.ppm
  env -u DISPLAY DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$d" \
      DQ8_GL="$gl" DQ8_RUN_GUEST_THREADS=1 DQ8_ELIDE_RASTER=0 \
      DQ8_USER_DATA_DIR="/home/hermes/.hermes/profiles/renderer/cache/scratch/ud_smoke" \
      "$BIN" DQ8.code dq8.romfs --load-state "$STATE" > "$d/run.log" 2>&1 &
  local gp=$! t=0
  while kill -0 $gp 2>/dev/null && [ $t -lt "$SECS" ]; do sleep 1; t=$((t+1)); done
  kill -TERM $gp 2>/dev/null; sleep 2; kill -KILL $gp 2>/dev/null
  echo "gl=$gl ran ${t}s; log tail:"; grep -E "GL backend|renderer|framebuffer fetch" "$d/run.log" | head -5
}
run 1
run 0
echo "--- md5"
md5sum "$OUT"/gl1/dq8-screen.ppm "$OUT"/gl0/dq8-screen.ppm 2>/dev/null
