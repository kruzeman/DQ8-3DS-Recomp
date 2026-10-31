#!/usr/bin/env bash
# usage: run7.sh <tag> <gl:0|1> <seconds>
# Post-fix GL-vs-software intro-movie comparison: existing (known-good) save
# userdata so both modes boot straight to the intro movie with NO input
# (an A press can skip a playing movie).
set -u
cd "$(dirname "$0")/.."
TAG=$1; GL=$2; SECS=${3:-90}
DIR=/home/hermes/.hermes/kanban/workspaces/t_667c2f8b/caps7/$TAG
mkdir -p "$DIR/frames"
: > "$DIR/run.log"
export DQ8_RUN_GUEST_THREADS=1 DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1
export DQ8_CAPTURE_PPM_DIR="$DIR"
export DQ8_USER_DATA_DIR="$DIR/userdata"
export DQ8_TRACE_LAYER=1
export DQ8_TRACE_Y2R=1
[ "$GL" = "1" ] && { export DQ8_GL=1 DQ8_GL_DEBUG=1; } || unset DQ8_GL
./target/dq8-gl DQ8.code dq8.romfs >> "$DIR/run.log" 2>&1 &
GP=$!
i=0
while kill -0 $GP 2>/dev/null; do
  sleep 1
  i=$((i+1))
  cp "$DIR/dq8-screen.ppm" "$DIR/frames/f_$(printf %03d $i).ppm" 2>/dev/null || true
  [ "$i" -ge "$SECS" ] && break
done
kill -9 $GP 2>/dev/null; wait $GP 2>/dev/null
echo "done $TAG frames=$(ls "$DIR/frames" | wc -l)"
