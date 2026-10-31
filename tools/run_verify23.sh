#!/usr/bin/env bash
# Fresh-userdata headless run to verify the SourceConfig-offset fix produces sound.
set -u
cd /home/hermes/DQ8/worktrees/audio || exit 1
BIN=target/dq8-startup-split
CODE=/home/hermes/DQ8/DQ8.code
ROMFS=/home/hermes/DQ8/target/dq8.romfs
LABEL=run23
DUR=150
PRESS=110
OUT=target/audio-evidence/$LABEL
UD=target/audio-ud-$LABEL
INPUT=$OUT/input.txt
rm -rf "$UD"; mkdir -p "$OUT/frames" "$UD"
printf '0\n' > "$INPUT"
: > "$OUT/run.log"; : > "$OUT/timeline.log"
echo "t0=$(date +%s)" > "$OUT/timeline.log"
DQ8_HEADLESS=1 DQ8_DISPLAY=0 DQ8_RUN_GUEST_THREADS=1 \
DQ8_TRACE_THREADS=1 DQ8_PROGRESS_SECONDS=1 \
DQ8_AUDIO_DUMP="$PWD/$OUT/audio.wav" \
DQ8_USER_DATA_DIR="$PWD/$UD" DQ8_INPUT_FILE="$PWD/$INPUT" \
"$BIN" "$CODE" "$ROMFS" > "$OUT/run.log" 2>&1 &
GP=$!
echo "pid=$GP" >> "$OUT/timeline.log"
for ((t=1; t<=DUR; t++)); do
  sleep 1
  if ! kill -0 "$GP" 2>/dev/null; then echo "exited at t=$t" >> "$OUT/timeline.log"; break; fi
  if (( t == PRESS )); then
    printf '1\n' > "$INPUT"; echo "t=$t A_DOWN" >> "$OUT/timeline.log"; sleep 1; printf '0\n' > "$INPUT"; echo "t=$t A_UP" >> "$OUT/timeline.log"
  fi
done
if kill -0 "$GP" 2>/dev/null; then kill -TERM "$GP" 2>/dev/null; sleep 3; kill -KILL "$GP" 2>/dev/null; wait "$GP" 2>/dev/null; fi
echo "done=$(date +%s)" >> "$OUT/timeline.log"
echo "run23 complete; wav:"; ls -la "$OUT/audio.wav" 2>/dev/null
