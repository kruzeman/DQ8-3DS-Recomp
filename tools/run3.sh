#!/usr/bin/env bash
# usage: run3.sh <tag> <gl:0|1> <seconds> [trace-flags...]
# caps3 runs: EXISTING save userdata (copied from the caps2 GL run) so the
# save-creation dialogs are skipped and both modes reach the intro movie
# with no input at all (no A press can skip a playing movie).
set -u
cd "$(dirname "$0")/.."
TAG=$1; GL=$2; SECS=${3:-120}
DIR=/home/hermes/.hermes/kanban/workspaces/t_667c2f8b/caps4/$TAG
mkdir -p "$DIR/frames"
: > "$DIR/run.log"
export DQ8_RUN_GUEST_THREADS=1 DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1
export DQ8_CAPTURE_PPM_DIR="$DIR"
export DQ8_USER_DATA_DIR="$DIR/userdata"
export DQ8_TRACE_LAYER=1
export DQ8_TRACE_Y2R=1
export DQ8_GL_DEBUG=1
[ "$GL" = "1" ] && export DQ8_GL=1 || unset DQ8_GL
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
