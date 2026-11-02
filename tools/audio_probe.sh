#!/usr/bin/env bash
# Bounded wall-clock audio probe for the DSP decoupling work (t_d808299b).
# Records the mixed audio sink (DQ8_AUDIO_DUMP) for SECONDS of wall time with
# an A-hold schedule, then SIGTERMs the runner.  Never reuses an evidence dir.
#
# usage: tools/audio_probe.sh LABEL RUNNER SECONDS [PRESS_TIMES]
#   PRESS_TIMES: comma separated wall seconds for a 1000 ms A press
#                (default "8,16,24,32,40,48,56,64,72,80")
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

LABEL=${1:?usage: audio_probe.sh LABEL RUNNER SECONDS [PRESS_TIMES]}
BIN=${2:?runner}
SECONDS_TOTAL=${3:-60}
PRESSES=${4:-8,16,24,32,40,48,56,64,72,80}
OUT="evidence/audio/$LABEL"
CODE=/home/hermes/DQ8/DQ8.code
ROMFS=/home/hermes/DQ8/target/dq8.romfs

[[ -e "$OUT" ]] && { echo "refusing to reuse evidence dir: $OUT" >&2; exit 2; }
[[ -x "$BIN" ]] || { echo "missing runner $BIN" >&2; exit 2; }
mkdir -p "$OUT/userdata"
printf '0\n' > "$OUT/input.txt"
printf 'label=%s seconds=%s start=%s\n' "$LABEL" "$SECONDS_TOTAL" "$(date +%s.%N)" > "$OUT/timeline.log"

env DQ8_HEADLESS=1 DQ8_DISPLAY=0 DQ8_RUN_GUEST_THREADS=1 \
    DQ8_PROGRESS_SECONDS=10 DQ8_TRACE_AUDIO=1 \
    DQ8_TRACE_DSP_HARVEST="${DQ8_TRACE_DSP_HARVEST:-}" \
    DQ8_TRACE_WAIT="${DQ8_TRACE_WAIT:-}" DQ8_TRACE_IPC="${DQ8_TRACE_IPC:-}" \
    DQ8_DSP_DUMP="${DQ8_DSP_DUMP:-}" \
    DQ8_AUDIO_DUMP="$PWD/$OUT/audio.wav" DQ8_AUDIO_SDL=1 SDL_AUDIODRIVER=dummy \
    DQ8_USER_DATA_DIR="$PWD/$OUT/userdata" DQ8_INPUT_FILE="$PWD/$OUT/input.txt" \
    "$BIN" "$CODE" "$ROMFS" > "$OUT/run.log" 2>&1 &
GP=$!
printf 'pid=%s bin=%s\n' "$GP" "$BIN" >> "$OUT/timeline.log"

press_at() {  # $1 = second
  printf 't=%s A_DOWN %s\n' "$1" "$(date +%s.%N)" >> "$OUT/timeline.log"
  printf '1\n' > "$OUT/input.txt"
  sleep 1
  printf '0\n' > "$OUT/input.txt"
  printf 't=%s A_UP %s\n' "$1" "$(date +%s.%N)" >> "$OUT/timeline.log"
}

START=$(date +%s)
for ((t = 1; t <= SECONDS_TOTAL; ++t)); do
  if ! kill -0 "$GP" 2>/dev/null; then
    printf 'runner exited early at t=%s\n' "$t" >> "$OUT/timeline.log"
    break
  fi
  if [[ ",$PRESSES," == *",$t,"* ]]; then
    press_at "$t"
  else
    sleep 1
  fi
done
if kill -0 "$GP" 2>/dev/null; then
  # SDL2 installs SIGINT/SIGTERM handlers for its quit event, so a graceful
  # kill is swallowed; the WAV header is refreshed every 256 frames and
  # tools/fix_wav_header.py repairs the tail, so hard-kill the runner.
  kill -9 "$GP" 2>/dev/null || true
  printf 'kill9_sent %s\n' "$(date +%s.%N)" >> "$OUT/timeline.log"
fi
set +e
wait "$GP"
RC=$?
set -e
printf 'exit=%s end=%s elapsed_s=%s\n' "$RC" "$(date +%s.%N)" "$(( $(date +%s) - START ))" >> "$OUT/timeline.log"
ls -l "$OUT/audio.wav" 2>/dev/null || true
