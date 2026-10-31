#!/usr/bin/env bash
# Walk through the boot flow to the multi-window naming screen by pressing A at
# intervals, capturing frames + DQ8_TRACE_TIME output. Fresh userdata auto-shows
# the save-data dialog, then the title, then the naming screen.
set -u
cd /home/hermes/DQ8
BIN=${BIN:-target/dq8-menu-perf-gl}
DUR=${1:-84}
LABEL=${2:-menu-walk}
MODE=${3:-sw}
OUT=target/title-evidence/$LABEL-$MODE
FRAMES=$OUT/frames
UD=$OUT/userdata
LOG=$OUT/run.log
PPM_DIR=$OUT/ppm
INPUT=$OUT/input.txt
mkdir -p "$FRAMES" "$UD" "$PPM_DIR"
: > "$LOG"; printf '0\n' > "$INPUT"

export DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_SKIP_MOVIE=1
export DQ8_TRACE_TIME=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$PPM_DIR"
export DQ8_USER_DATA_DIR="$UD" DQ8_INPUT_FILE="$INPUT"
[ "$MODE" = gl ] && export DQ8_GL=1

echo "t0=$(date +%s) mode=$MODE dur=$DUR" > "$OUT/timeline.log"
"$BIN" DQ8.code target/dq8.romfs > "$LOG" 2>&1 &
GP=$!
echo "pid=$GP" >> "$OUT/timeline.log"
# Press A (0x1) for 1000ms at t=12,20,28,36,44 — enough to walk dialog->title->menu->naming.
press_at="12 20 28 36 44"
for ((t=1; t<=DUR; t++)); do
  sleep 1
  kill -0 $GP 2>/dev/null || { echo "exited t=$t" >> "$OUT/timeline.log"; break; }
  for p in $press_at; do
    if (( t == p )); then
      printf '1\n' > "$INPUT"
      echo "t=$t A_DOWN" >> "$OUT/timeline.log"
      cp -f "$PPM_DIR"/dq8-screen.ppm "$FRAMES/t$(printf %03d $t)_down.ppm" 2>/dev/null || true
      sleep 1
      printf '0\n' > "$INPUT"
      echo "t=$t A_UP" >> "$OUT/timeline.log"
    fi
  done
  if (( t % 8 == 0 )); then
    cp -f "$PPM_DIR"/dq8-screen.ppm "$FRAMES/t$(printf %03d $t).ppm" 2>/dev/null || true
  fi
done
kill -KILL $GP 2>/dev/null; wait $GP 2>/dev/null
echo "done=$(date +%s)" >> "$OUT/timeline.log"
echo "=== $LABEL-$MODE timeline ==="; cat "$OUT/timeline.log"
echo "=== frames ==="; ls "$FRAMES"
