#!/usr/bin/env bash
# Measure translucent-window (RMW blend) cost on the auto-reached naming screen
# (3 stacked semi-transparent windows). Uses DQ8_TRACE_TIME for per-draw
# classification and captures PPM frames to confirm the scene.
set -u
cd /home/hermes/DQ8
BIN=${BIN:-target/dq8-menu-perf-gl}
DUR=${1:-60}
LABEL=${2:-menu-perf}
MODE=${3:-sw}          # sw | gl
OUT=target/title-evidence/$LABEL-$MODE
FRAMES=$OUT/frames
UD=$OUT/userdata
LOG=$OUT/run.log
PPM_DIR=$OUT/ppm
mkdir -p "$FRAMES" "$UD" "$PPM_DIR"
: > "$LOG"

export DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_SKIP_MOVIE=1
export DQ8_TRACE_TIME=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$PPM_DIR"
export DQ8_USER_DATA_DIR="$UD"
[ "$MODE" = gl ] && export DQ8_GL=1

echo "t0=$(date +%s) mode=$MODE dur=$DUR" > "$OUT/timeline.log"
"$BIN" DQ8.code target/dq8.romfs > "$LOG" 2>&1 &
GP=$!
echo "pid=$GP" >> "$OUT/timeline.log"
for ((t=1; t<=DUR; t++)); do
  sleep 1
  kill -0 $GP 2>/dev/null || { echo "exited t=$t" >> "$OUT/timeline.log"; break; }
  if (( t % 10 == 0 )); then
    cp -f "$PPM_DIR"/dq8-screen.ppm "$FRAMES/t$(printf %03d $t).ppm" 2>/dev/null || true
  fi
done
kill -KILL $GP 2>/dev/null; wait $GP 2>/dev/null
echo "done=$(date +%s)" >> "$OUT/timeline.log"
echo "=== $LABEL-$MODE: TIME summary (RMW = translucent) ==="
grep "^TIME" "$LOG" | tail -40
