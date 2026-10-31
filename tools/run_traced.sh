#!/usr/bin/env bash
# Instrumented GL run through the movie window (fixed press schedule known to
# dismiss the dialog: t=3,9,15). Heavy tracing only during the whole run.
set -u
cd "$(dirname "$0")/.."
TAG=$1; GL=$2; SECS=${3:-240}
DIR=/home/hermes/.hermes/kanban/workspaces/t_667c2f8b/caps2/$TAG
mkdir -p "$DIR/frames" "$DIR/userdata"
: > "$DIR/run.log"
INPUT=$DIR/input.txt
printf '0\n' > "$INPUT"
export DQ8_RUN_GUEST_THREADS=1 DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1
export DQ8_CAPTURE_PPM_DIR="$DIR"
export DQ8_USER_DATA_DIR="$DIR/userdata"
export DQ8_INPUT_FILE="$INPUT"
export DQ8_TRACE_INPUT=1
export DQ8_TRACE_LAYER=1
export DQ8_TRACE_Y2R=1
export DQ8_GL_DEBUG=1
[ "$GL" = "1" ] && export DQ8_GL=1
./target/dq8-gl DQ8.code dq8.romfs >> "$DIR/run.log" 2>&1 &
GP=$!
i=0
while kill -0 $GP 2>/dev/null; do
  sleep 1
  i=$((i+1))
  cp "$DIR/dq8-screen.ppm" "$DIR/frames/f_$(printf %03d $i).ppm" 2>/dev/null || true
  if (( i <= 18 )); then
    if (( i % 6 == 3 )); then printf '1\n' > "$INPUT"; fi
    if (( i % 6 == 4 )); then printf '0\n' > "$INPUT"; fi
  fi
  [ "$i" -ge "$SECS" ] && break
done
printf '0\n' > "$INPUT"
kill -9 $GP 2>/dev/null; wait $GP 2>/dev/null
echo "done $TAG frames=$(ls "$DIR/frames" | wc -l)"
