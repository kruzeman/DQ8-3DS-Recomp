#!/usr/bin/env bash
export DQ8_CAPTURE_PPM=1   # PPM screen snapshots are opt-in since audit 5.4
# run28 = run27 scenario (gates build) + frame snapshots (/tmp/dq8-screen.ppm)
# + IPC trace for FS/save-stage evidence.
set -u
cd /home/hermes/DQ8/worktrees/audio || exit 1
BIN=target/dq8-startup-split
CODE=/home/hermes/DQ8/DQ8.code
ROMFS=/home/hermes/DQ8/target/dq8.romfs
LABEL=run28
DUR=150
PRESS=110
OUT=target/audio-evidence/$LABEL
UD=target/audio-ud-$LABEL
INPUT=$OUT/input.txt
FRAMES=$OUT/frames
PPM_FULL=288015
rm -rf "$UD" "$OUT"; mkdir -p "$FRAMES" "$OUT/se" "$UD"
printf '0\n' > "$INPUT"
: > "$OUT/run.log"; : > "$OUT/timeline.log"
echo "t0=$(date +%s)" > "$OUT/timeline.log"
snapshot() {
    local i size
    for i in 1 2 3 4 5; do
        if [ -f /tmp/dq8-screen.ppm ]; then
            cp -f /tmp/dq8-screen.ppm "$FRAMES/$1.ppm" 2>/dev/null || true
            size=$(stat -c %s "$FRAMES/$1.ppm" 2>/dev/null || echo 0)
            if [ "$size" -ge "$PPM_FULL" ]; then return 0; fi
        fi
        sleep 0.2
    done
    rm -f "$FRAMES/$1.ppm"
    return 1
}
DQ8_HEADLESS=1 DQ8_DISPLAY=0 DQ8_RUN_GUEST_THREADS=1 \
DQ8_TRACE_AUDIO=1 DQ8_TRACE_IPC=1 DQ8_PROGRESS_SECONDS=1 \
DQ8_AUDIO_DUMP="$PWD/$OUT/audio.wav" \
DQ8_DSP_SE_DIR="$PWD/$OUT/se" \
DQ8_USER_DATA_DIR="$PWD/$UD" DQ8_INPUT_FILE="$PWD/$INPUT" \
"$BIN" "$CODE" "$ROMFS" > "$OUT/run.log" 2>&1 &
GP=$!
echo "pid=$GP" >> "$OUT/timeline.log"
for ((t=1; t<=DUR; t++)); do
  sleep 1
  if ! kill -0 "$GP" 2>/dev/null; then echo "exited at t=$t" >> "$OUT/timeline.log"; break; fi
  if (( t % 5 == 0 )); then snapshot "t$(printf '%04d' "$t")"; fi
  if (( t == PRESS )); then
    printf '1\n' > "$INPUT"; echo "t=$t A_DOWN $(date +%s)" >> "$OUT/timeline.log"
    snapshot "t$(printf '%04d' "$t")_press_down"; sleep 1
    printf '0\n' > "$INPUT"; echo "t=$t A_UP $(date +%s)" >> "$OUT/timeline.log"
    snapshot "t$(printf '%04d' "$t")_press_up"
  fi
done
if kill -0 "$GP" 2>/dev/null; then snapshot "t9999_final"; kill -TERM "$GP" 2>/dev/null; sleep 3; kill -KILL "$GP" 2>/dev/null; wait "$GP" 2>/dev/null; fi
echo "done=$(date +%s)" >> "$OUT/timeline.log"
echo "$LABEL complete"
