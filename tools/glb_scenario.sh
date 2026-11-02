#!/usr/bin/env bash
# t_d559a510: scripted-input A/B capture at the operator's save-menu savestate.
# usage: tools/glb_scenario.sh <label> <outdir> <spec...> [ENV=VAL ...]
#
#   <spec>   <second>:<mask>   hold that HID mask for 1000 ms starting at that
#                              wall-clock second (0x80=Down, 0x1=A, 0x40=Up,
#                              0x20=Left, 0x10=Right, 0x8=Start).
#
# Default scenario for the operator's repro (reaches the name-entry keyboard):
#   5:0x80 (Down -> "Create a new adventure log")  7:0x1 (A)  11:0x1 (A)
#
# Every distinct top-screen PPM frame is stored as f_NNN_<md5>.ppm;
# frames.timeline records the capture second and the input edges so two arms
# can be aligned by phase.
set -u
LABEL=${1:?label}; OUT=${2:?outdir}; shift 2
SPECS=(); EXTRA=()
for a in "$@"; do
  case "$a" in *=*) EXTRA+=("$a");; *) SPECS+=("$a");; esac
done
if [ ${#SPECS[@]} -eq 0 ]; then SPECS=(5:0x80 7:0x1 11:0x1); fi
SECS=${SECS:-26}
W=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=${BIN:-$W/target/dq8-glblend}
UD=/home/hermes/.hermes/profiles/coder/cache/scratch/glb_ud_sc
IN=${IN:-/tmp/glb_input.txt}
mkdir -p "$OUT" "$UD"
rm -f "$OUT"/f_*.ppm "$OUT"/dq8-screen.ppm
echo 0 > "$IN"
cd "$W"
env DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT" \
    DQ8_TRACE_TIME=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
    DQ8_INPUT_FILE="$IN" \
    DQ8_USER_DATA_DIR="$UD" "${EXTRA[@]}" \
    "$BIN" DQ8.code dq8.romfs \
      --load-state /home/hermes/DQ8/saves/dq8-state.sav > "$OUT/run.log" 2>&1 &
GP=$!
T0=$(date +%s)
: > "$OUT/frames.timeline"
last=""; n=0; si=0; released=1
# Expand every spec into a press at `at` and a release one second later.
EV=()
for s in "${SPECS[@]}"; do
  at=${s%%:*}; mask=${s#*:}
  EV+=("$at:$mask" "$((at+1)):0")
done
evi=0
while [ $(( $(date +%s) - T0 )) -lt "$SECS" ]; do
  el=$(( $(date +%s) - T0 ))
  if [ $evi -lt ${#EV[@]} ]; then
    at=${EV[$evi]%%:*}; mask=${EV[$evi]#*:}
    if [ "$el" -ge "$at" ]; then
      echo "$mask" > "$IN"; echo "t=$el set mask=$mask" >> "$OUT/frames.timeline"
      evi=$((evi+1))
    fi
  fi
  f="$OUT/dq8-screen.ppm"
  if [ -f "$f" ] && [ "$(stat -c %s "$f")" -gt 200000 ]; then
    h=$(md5sum "$f" | cut -d' ' -f1)
    if [ "$h" != "$last" ]; then
      n=$((n+1)); cp "$f" "$(printf '%s/f_%03d_%s.ppm' "$OUT" "$n" "$h")"; last="$h"
      echo "t=$el f_$(printf '%03d' $n)_$h.ppm" >> "$OUT/frames.timeline"
    fi
  fi
  sleep 0.2
done
echo 0 > "$IN"
kill -9 $GP 2>/dev/null; wait $GP 2>/dev/null
echo "$LABEL distinct_frames=$n last_md5=$(md5sum "$OUT"/dq8-screen.ppm 2>/dev/null | cut -d' ' -f1)"
grep -oE 'blend_gl=[0-9]+ blend_sw=[0-9]+' "$OUT/run.log" | tail -1 | sed "s/^/$LABEL /"
grep -oE 'texbar=[0-9]+' "$OUT/run.log" | tail -1 | sed "s/^/$LABEL /"
grep -c 'HID buttons' "$OUT/run.log" | sed "s/^/$LABEL hid_edges=/"
