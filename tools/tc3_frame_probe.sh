#!/usr/bin/env bash
# t_66c89cf2: run a binary and snapshot the screen at an exact FRAME count
# (cum frames in FRAMESTAT), not wall time — removes animation-phase skew
# between runs of different speed.
# usage: tc3_frame_probe.sh <bin> <out_prefix> <target_frame> [extra_env]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.."
BIN=$1
OUT=$2
TARGET=$3
EXTRA=${4:-}
CODE=/home/hermes/DQ8/DQ8.code
ROMFS=/home/hermes/DQ8/target/dq8.romfs
STATE=/home/hermes/DQ8/saves/dq8-state.sav
HONEY=/home/hermes/DQ8/saves/honeySave.bin
DUR=${DUR:-60}

DIR=target/blackworld-evidence/$OUT
mkdir -p "$DIR/frames"
rm -rf "$DIR/userdata"; mkdir -p "$DIR/userdata/archive-00000004-00000000"
cp -f "$HONEY" "$DIR/userdata/archive-00000004-00000000/honeySave.bin"
printf '0\n' > "$DIR/input.txt"
: > "$DIR/run.log"

EXTRA_ENV=()
[ -n "$EXTRA" ] && read -ra EXTRA_ENV <<< "$EXTRA"

env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
    DQ8_PROGRESS_SECONDS=10 DQ8_CAPTURE_PPM=1 \
    DQ8_CAPTURE_PPM_DIR="$DIR" \
    DQ8_FRAME_STATS=1 DQ8_GL=1 "${EXTRA_ENV[@]}" \
    DQ8_USER_DATA_DIR="$DIR/userdata" DQ8_INPUT_FILE="$DIR/input.txt" \
    "./$BIN" "$CODE" "$ROMFS" --load-state "$STATE" > "$DIR/run.log" 2>&1 &
gp=$!

snap=0
n=0
while sleep 1; do
  n=$((n+1))
  if ! kill -0 "$gp" 2>/dev/null; then echo "exited at t=$n" >> "$DIR/timeline.log"; break; fi
  # latest cumulative frame count from FRAMESTAT
  cf=$(grep -o 'cum: frames=[0-9]*' "$DIR/run.log" | tail -1 | cut -d= -f2)
  cf=${cf:-0}
  if [ "$snap" = 0 ] && [ "$cf" -ge "$TARGET" ]; then
    cp -f "$DIR/dq8-screen.ppm" "$DIR/frames/f$(printf '%04d' "$TARGET").ppm"
    echo "t=$n frames=$cf snapshot f$TARGET" >> "$DIR/timeline.log"
    snap=1
    break
  fi
  [ "$n" -ge "$DUR" ] && { echo "timeout frames=$cf" >> "$DIR/timeline.log"; break; }
done
kill "$gp" 2>/dev/null
for _ in 1 2 3; do kill -0 "$gp" 2>/dev/null || break; sleep 1; done
kill -9 "$gp" 2>/dev/null
wait "$gp" 2>/dev/null
cf=$(grep -o 'cum: frames=[0-9]*' "$DIR/run.log" | tail -1 | cut -d= -f2)
echo "final frames=${cf:-?} snap=$snap" >> "$DIR/timeline.log"
ls -la "$DIR/frames/" >> "$DIR/timeline.log"
echo "OUT=$DIR"
