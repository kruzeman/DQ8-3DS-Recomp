#!/usr/bin/env bash
# t_d559a510: A/B capture at the save-menu savestate.
# usage: tools/glb_verify.sh <label> <outdir> [seconds] [ENV=VAL ...]
# Captures every distinct top-screen PPM frame to <outdir>/f_NNN_<md5>.ppm and
# keeps run.log (GLPHASE / blend counters) so two arms can be frame-diffed.
set -u
LABEL=${1:?label}; OUT=${2:?outdir}; SECS=${3:-30}; shift 3
W=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=${BIN:-$W/target/dq8-glblend}
UD=/home/hermes/.hermes/profiles/coder/cache/scratch/glb_ud
mkdir -p "$OUT" "$UD"
rm -f "$OUT"/f_*.ppm "$OUT"/dq8-screen.ppm
cd "$W"
env DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT" \
    DQ8_TRACE_TIME=1 DQ8_RUN_GUEST_THREADS=1 DQ8_USER_DATA_DIR="$UD" "$@" \
    "$BIN" DQ8.code dq8.romfs \
      --load-state /home/hermes/DQ8/saves/dq8-state.sav > "$OUT/run.log" 2>&1 &
GP=$!
last=""; n=0; end=$(( $(date +%s) + SECS ))
while [ "$(date +%s)" -lt "$end" ]; do
  f="$OUT/dq8-screen.ppm"
  if [ -f "$f" ] && [ "$(stat -c %s "$f")" -gt 200000 ]; then
    h=$(md5sum "$f" | cut -d' ' -f1)
    if [ "$h" != "$last" ]; then
      n=$((n+1)); cp "$f" "$(printf '%s/f_%03d_%s.ppm' "$OUT" "$n" "$h")"; last=$h
    fi
  fi
  sleep 0.2
done
kill -9 $GP 2>/dev/null; wait $GP 2>/dev/null
echo "$LABEL distinct_frames=$n last_md5=$(md5sum "$OUT"/dq8-screen.ppm 2>/dev/null | cut -d' ' -f1)"
grep -oE 'blend_gl=[0-9]+ blend_sw=[0-9]+' "$OUT/run.log" | tail -1 | sed "s/^/$LABEL /"
grep -oE 'texbar=[0-9]+' "$OUT/run.log" | tail -1 | sed "s/^/$LABEL /"
grep -oE 'framebuffer fetch \(imageLoad\) = [a-z]+' "$OUT/run.log" | tail -1 | sed "s/^/$LABEL /"
grep -oE 'BLENDFAIL[^\n]*' "$OUT/run.log" | head -5
