#!/usr/bin/env bash
# Isolated idle-dialog CPU sample for H2. Use fresh userdata; set
# DQ8_IDLE_PRESS_ATS=none for the no-input scenario. Optional PPM capture is
# redirected into this run's evidence directory.
#
# LEGACY-MEASUREMENT WARNING (decision reports/PERF_IDLE_DIALOG_BGM_DECISION.md,
# audit IDLE-01/IDLE-02): this helper (1) hardcodes DQ8_AUDIO_SDL=0 below, so
# every run it produces is a MUTED run (host sink disabled) — never an
# audible/streaming-BGM one, and (2) records `ps -L ... pcpu`, a LIFETIME
# per-thread average, not a delta over the measurement window. Do not read its
# output as interval CPU or as an audible baseline. For t=160..190 interval CPU
# use tools/run_idle_scenario.py (utime+stime deltas from /proc task stat) and
# record the sink state alongside the numbers.
set -euo pipefail

LABEL=${1:?usage: run_h2_idle_dialog_capture.sh LABEL [RUNNER]}
BIN=${2:-target/dq8-audio-workers}
OUT="target/audio-workers-evidence/$LABEL"
CODE=/home/hermes/DQ8/DQ8.code
ROMFS=/home/hermes/DQ8/target/dq8.romfs
MAX_WALL=${DQ8_IDLE_CAPTURE_MAX_WALL:-190}
PRESS_TIMES=${DQ8_IDLE_PRESS_ATS:-30,60,90}
CAPTURE_PPM=${DQ8_IDLE_CAPTURE_PPM:-0}
CAPTURE_TIMES=${DQ8_IDLE_CAPTURE_TIMES:-155,185}

if [[ -e "$OUT" ]]; then
  echo "refusing to reuse evidence directory: $OUT" >&2
  exit 2
fi
if [[ ! -x "$BIN" || ! -f "$CODE" || ! -f "$ROMFS" ]]; then
  echo "missing runner or read-only local DQ8 inputs" >&2
  exit 2
fi
mkdir -p "$OUT/userdata"
if [[ "$PRESS_TIMES" == "none" ]]; then
  PRESS_ATS=()
else
  IFS=, read -r -a PRESS_ATS <<< "$PRESS_TIMES"
fi
IFS=, read -r -a CAPTURE_ATS <<< "$CAPTURE_TIMES"
if [[ "$CAPTURE_PPM" == "1" ]]; then
  mkdir -p "$OUT/frames"
  BENCH_NO_SNAPSHOT=0
else
  BENCH_NO_SNAPSHOT=1
fi
INPUT="$PWD/$OUT/input.txt"
printf '0\n' > "$INPUT"
printf 'label=%s input_schedule=%s capture_times=%s start=%s\n' "$LABEL" "$PRESS_TIMES" "$CAPTURE_TIMES" "$(date +%s.%N)" > "$OUT/timeline.log"
: > "$OUT/thread-cpu.log"

DQ8_HEADLESS=1 DQ8_DISPLAY=0 DQ8_RUN_GUEST_THREADS=1 \
DQ8_BENCH_NO_SNAPSHOT="$BENCH_NO_SNAPSHOT" DQ8_PROGRESS_SECONDS=10 DQ8_AUDIO_SDL=0 \
DQ8_CAPTURE_PPM="$CAPTURE_PPM" DQ8_CAPTURE_PPM_DIR="$PWD/$OUT/frames" \
DQ8_TRACE_INPUT=1 DQ8_INPUT_FILE="$INPUT" \
DQ8_USER_DATA_DIR="$PWD/$OUT/userdata" \
"$BIN" "$CODE" "$ROMFS" > "$OUT/run.log" 2>&1 &
GP=$!
printf 'pid=%s\n' "$GP" >> "$OUT/timeline.log"
START=$(date +%s)
HARNESS_TERMINATED=0
ELAPSED=0
while (( ELAPSED < MAX_WALL )); do
  sleep 1
  ELAPSED=$(( $(date +%s) - START ))
  if ! kill -0 "$GP" 2>/dev/null; then break; fi
  printf 't=%s\n' "$ELAPSED" >> "$OUT/thread-cpu.log"
  ps -L -p "$GP" -o tid=,pcpu=,comm= >> "$OUT/thread-cpu.log" 2>/dev/null || true
  for PRESS_AT in "${PRESS_ATS[@]}"; do
    if (( ELAPSED == PRESS_AT )); then
      printf 't=%s A_DOWN %s\n' "$ELAPSED" "$(date +%s.%N)" >> "$OUT/timeline.log"
      printf '1\n' > "$INPUT"
      sleep 1
      printf '0\n' > "$INPUT"
      printf 't=%s A_UP %s\n' "$ELAPSED" "$(date +%s.%N)" >> "$OUT/timeline.log"
    fi
  done
  if [[ "$CAPTURE_PPM" == "1" ]]; then
    for CAPTURE_AT in "${CAPTURE_ATS[@]}"; do
      if (( ELAPSED != CAPTURE_AT )); then continue; fi
      for SCREEN in dq8-screen.ppm dq8-screen-bottom.ppm; do
        if [[ -f "$OUT/frames/$SCREEN" ]]; then
          cp "$OUT/frames/$SCREEN" "$OUT/frames/t${ELAPSED}-${SCREEN}"
          printf 't=%s captured=%s mtime=%s\n' "$ELAPSED" "$SCREEN" "$(stat -c %y "$OUT/frames/$SCREEN")" >> "$OUT/timeline.log"
        else
          printf 't=%s missing=%s\n' "$ELAPSED" "$SCREEN" >> "$OUT/timeline.log"
        fi
      done
    done
  fi
done
if kill -0 "$GP" 2>/dev/null; then
  printf 'idle-window sample complete; sending TERM at t=%s\n' "$ELAPSED" >> "$OUT/timeline.log"
  HARNESS_TERMINATED=1
  kill -TERM "$GP" 2>/dev/null || true
fi
set +e
wait "$GP"
RC=$?
set -e
printf 'exit=%s end=%s elapsed_s=%s\n' "$RC" "$(date +%s.%N)" "$(( $(date +%s) - START ))" >> "$OUT/timeline.log"
printf 'capture exit=%s log=%s cpu=%s\n' "$RC" "$OUT/run.log" "$OUT/thread-cpu.log"
if (( HARNESS_TERMINATED )) && (( RC == 143 || RC == 0 )); then exit 0; fi
exit "$RC"
