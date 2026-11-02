#!/usr/bin/env bash
# t_6b81015d repro capture v2: distinct PPM frames from the save-menu savestate,
# hard-kills the guest (it ignores SIGINT).
# usage: cap_menu2.sh <outdir> <seconds> [env assignments...]
set -u
W=/home/hermes/DQ8/worktrees/texcache2-t_66c89cf2
OUT=${1:?outdir}; SECS=${2:-20}; shift 2
mkdir -p "$OUT"; rm -f "$OUT"/f_*.ppm "$OUT"/b_*.ppm
cd "$W"
UD=/home/hermes/.hermes/profiles/coder/cache/scratch/t_6b81015d_userdata
mkdir -p "$UD"
: > "$OUT/run.log"
env DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT" \
    DQ8_USER_DATA_DIR="$UD" "$@" \
    ./target/dq8-perfsplit1 DQ8.code dq8.romfs \
      --load-state /home/hermes/DQ8/saves/dq8-state.sav >> "$OUT/run.log" 2>&1 &
GP=$!
last=""; n=0; end=$(( $(date +%s) + SECS ))
while [ "$(date +%s)" -lt "$end" ]; do
  f="$OUT/dq8-screen.ppm"
  if [ -f "$f" ] && [ "$(stat -c %s "$f")" -gt 200000 ]; then
    h=$(md5sum "$f" | cut -d' ' -f1)
    if [ "$h" != "$last" ]; then
      n=$((n+1))
      cp "$f" "$(printf '%s/f_%03d_%s.ppm' "$OUT" "$n" "$h")"
      last=$h
    fi
  fi
  sleep 0.25
done
kill -9 $GP 2>/dev/null
wait $GP 2>/dev/null
echo "distinct_frames=$n out=$OUT"
