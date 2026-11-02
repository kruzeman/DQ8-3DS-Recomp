#!/usr/bin/env bash
# One-shot menu frame capture: runs the savestate for N seconds, saves the LAST
# PPM as <out>.ppm, then hard-kills the process (the guest ignores SIGINT).
# usage: cap1.sh <out.ppm> <seconds> [env assignments...]
set -u
W=/home/hermes/DQ8/worktrees/texcache2-t_66c89cf2
OUT=${1:?out}; SECS=${2:-10}; BIN=${3:-./target/dq8-perfsplit1}; shift 3
D=$(dirname "$OUT"); mkdir -p "$D"
UD=/home/hermes/.hermes/profiles/coder/cache/scratch/t_6b81015d_userdata
mkdir -p "$UD"
cd "$W"
TMPD=$(mktemp -d /tmp/t6capXXXX)
env DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$TMPD" \
    DQ8_USER_DATA_DIR="$UD" "$@" \
    "$BIN" DQ8.code dq8.romfs \
      --load-state /home/hermes/DQ8/saves/dq8-state.sav > "$D/$(basename "$OUT" .ppm).log" 2>&1 &
GP=$!
sleep "$SECS"
# copy a *complete* frame (the writer truncates then rewrites; size must be stable)
for i in $(seq 1 40); do
  s1=$(stat -c %s "$TMPD/dq8-screen.ppm" 2>/dev/null || echo 0)
  sleep 0.15
  s2=$(stat -c %s "$TMPD/dq8-screen.ppm" 2>/dev/null || echo 0)
  if [ "$s1" = "$s2" ] && [ "$s1" -gt 200000 ]; then break; fi
done
if [ -f "$TMPD/dq8-screen.ppm" ]; then cp "$TMPD/dq8-screen.ppm" "$OUT"; \
  [ -f "$TMPD/dq8-screen-bottom.ppm" ] && cp "$TMPD/dq8-screen-bottom.ppm" "${OUT%.ppm}_bot.ppm"; fi
kill -9 $GP 2>/dev/null
wait $GP 2>/dev/null
md5sum "$OUT" 2>/dev/null || echo "NO FRAME"
rm -rf "$TMPD"
