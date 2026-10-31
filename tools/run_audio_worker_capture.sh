#!/usr/bin/env bash
# Reproducible fresh-save movie audio/CPU capture for H2. Never reuses or
# deletes user data; each label must name a new isolated evidence directory.
set -euo pipefail

LABEL=${1:?usage: run_audio_worker_capture.sh LABEL [RUNNER] [STEPS]}
BIN=${2:-target/dq8-audio-workers}
STEPS=${3:-300000000}
OUT="target/audio-workers-evidence/$LABEL"
CODE=/home/hermes/DQ8/DQ8.code
ROMFS=/home/hermes/DQ8/target/dq8.romfs

if [[ -e "$OUT" ]]; then
  echo "refusing to reuse evidence directory: $OUT" >&2
  exit 2
fi
if [[ ! -x "$BIN" || ! -f "$CODE" || ! -f "$ROMFS" ]]; then
  echo "missing runner or read-only local DQ8 inputs" >&2
  exit 2
fi
mkdir -p "$OUT/userdata"
INPUT="$PWD/$OUT/input.txt"
AUDIO="$PWD/$OUT/audio.wav"
LOG="$PWD/$OUT/run.log"
printf '0\n' > "$INPUT"
: > "$OUT/thread-cpu.log"
printf 'label=%s steps=%s start=%s\n' "$LABEL" "$STEPS" "$(date +%s.%N)" > "$OUT/timeline.log"

DQ8_HEADLESS=1 DQ8_DISPLAY=0 DQ8_RUN_GUEST_THREADS=1 \
DQ8_BENCH_STEPS="$STEPS" DQ8_BENCH_NO_SNAPSHOT=1 DQ8_PROGRESS_SECONDS=10 \
DQ8_TRACE_AUDIO=1 DQ8_TRACE_IPC=1 DQ8_TRACE_INPUT=1 \
DQ8_AUDIO_DUMP="$AUDIO" DQ8_AUDIO_SDL=1 SDL_AUDIODRIVER=dummy \
DQ8_USER_DATA_DIR="$PWD/$OUT/userdata" DQ8_INPUT_FILE="$INPUT" \
"$BIN" "$CODE" "$ROMFS" > "$LOG" 2>&1 &
GP=$!
printf 'pid=%s\n' "$GP" >> "$OUT/timeline.log"
START=$(date +%s)
MAX_WALL=${DQ8_CAPTURE_MAX_WALL:-1200}
for ((t = 1; t <= MAX_WALL; ++t)); do
  sleep 1
  if ! kill -0 "$GP" 2>/dev/null; then break; fi
  printf 't=%s\n' "$t" >> "$OUT/thread-cpu.log"
  ps -L -p "$GP" -o tid=,pcpu=,comm= >> "$OUT/thread-cpu.log" 2>/dev/null || true
  if (( t == 30 || t == 60 || t == 90 )); then
    printf 't=%s A_DOWN %s\n' "$t" "$(date +%s.%N)" >> "$OUT/timeline.log"
    printf '1\n' > "$INPUT"
    sleep 1
    printf '0\n' > "$INPUT"
    printf 't=%s A_UP %s\n' "$t" "$(date +%s.%N)" >> "$OUT/timeline.log"
  fi
done
if kill -0 "$GP" 2>/dev/null; then
  printf 'step budget exceeded max wall %ss; sending TERM\n' "$MAX_WALL" >> "$OUT/timeline.log"
  kill -TERM "$GP" 2>/dev/null || true
fi
set +e
wait "$GP"
RC=$?
set -e
printf 'exit=%s end=%s elapsed_s=%s\n' "$RC" "$(date +%s.%N)" "$(( $(date +%s) - START ))" >> "$OUT/timeline.log"
printf 'capture exit=%s log=%s audio=%s\n' "$RC" "$LOG" "$AUDIO"
exit "$RC"
