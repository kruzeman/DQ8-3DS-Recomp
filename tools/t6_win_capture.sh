#!/usr/bin/env bash
# t_6b81015d: WINDOWED run of the savestate on Xvfb :99 (operator configuration),
# grabs N frames of the game window with x11grab.
# usage: t6_win_capture.sh <outdir> <seconds> [env assignments...]
set -u
W=/home/hermes/DQ8/worktrees/texcache2-t_66c89cf2
OUT=${1:?outdir}; SECS=${2:-15}; shift 2
mkdir -p "$OUT"; rm -f "$OUT"/w_*.png
cd "$W"
UD=/home/hermes/.hermes/profiles/coder/cache/scratch/t_6b81015d_userdata_win
mkdir -p "$UD"
export DISPLAY=:98
pgrep -f "Xvfb :98" >/dev/null || (Xvfb :98 -screen 0 1600x1200x24 >/tmp/t6/xvfb98.log 2>&1 &)
sleep 1
: > "$OUT/run.log"
env DQ8_RUN_GUEST_THREADS=1 SDL_VIDEODRIVER=x11 DQ8_USER_DATA_DIR="$UD" "$@" \
  ./target/dq8-perfsplit1 DQ8.code dq8.romfs \
    --load-state /home/hermes/DQ8/saves/dq8-state.sav >> "$OUT/run.log" 2>&1 &
GP=$!
sleep 6
# find the game window, grab its exact area
WID=$(xdotool search --name "DQ8" | head -1)
if [ -z "${WID:-}" ]; then WID=$(xdotool search --name "dq8" | head -1); fi
echo "wid=${WID:-none}" >> "$OUT/run.log"
eval "$(xdotool getwindowgeometry --shell "$WID" 2>/dev/null)" || true
echo "geo X=$X Y=$Y W=$WIDTH H=$HEIGHT" >> "$OUT/run.log"
GEO="${X:-0},${Y:-0}"
SIZE="${WIDTH:-800}x${HEIGHT:-960}"
for i in $(seq 1 6); do
  ffmpeg -v error -f x11grab -video_size "$SIZE" -i ":98+$GEO" -frames:v 1 "$OUT/w_$i.png" -y
  sleep 1
done
kill -9 $GP 2>/dev/null; wait $GP 2>/dev/null
ls -la "$OUT" | head
