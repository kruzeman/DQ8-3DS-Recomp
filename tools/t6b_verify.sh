#!/usr/bin/env bash
# t_6b81015d: A/B capture at the adventure-log menu (savestate boot).
# usage: t6b_verify.sh <label> <binary> <outdir> [seconds] [env...]
# Captures distinct top-screen PPM frames + the GLPHASE line, so frame identity
# (md5) and the texbar counter can be compared between two binaries/backends.
set -u
LABEL=${1:?label}; BIN=${2:?binary}; OUT=${3:?outdir}; SECS=${4:-25}; shift 4
W=/home/hermes/DQ8/worktrees/rendermenu-t_6b81015d
UD=/home/hermes/.hermes/profiles/coder/cache/scratch/t6_ud_verify
mkdir -p "$OUT" "$UD"
rm -f "$OUT"/f_*.ppm "$OUT"/dq8-screen.ppm
cd "$W"
env DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT" \
    DQ8_TRACE_TIME=1 DQ8_USER_DATA_DIR="$UD" "$@" \
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
  sleep 0.25
done
kill -9 $GP 2>/dev/null; wait $GP 2>/dev/null
echo "$LABEL distinct_frames=$n md5=$(md5sum "$OUT"/dq8-screen.ppm 2>/dev/null | cut -d' ' -f1)"
grep -oE 'texbar=[0-9]+' "$OUT/run.log" | tail -1 | sed "s/^/$LABEL /"
grep -oE 'blend_sw=[0-9]+' "$OUT/run.log" | tail -1 | sed "s/^/$LABEL /"
grep -oE 'framebuffer fetch \(imageLoad\) = [a-z]+' "$OUT/run.log" | tail -1 | sed "s/^/$LABEL /"
