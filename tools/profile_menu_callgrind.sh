#!/usr/bin/env bash
# Callgrind CPU attribution on the MAIN MENU scene: fresh boot auto-shows the
# save-data dialog; A presses dismiss it and land on the Adventure-Log menu,
# where the top screen dims behind a full-screen semi-transparent scrim (the
# translucent-window draw that lags).  Instrumentation is OFF during boot and
# ON for a bounded window once the dimmed main-menu title is up.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
BIN=${BIN:-target/dq8-menu-perf-gl}
OUT=${OUT:-target/title-evidence/menu-prof-sw/callgrind.out}
PRE=${PRE:-30}
WIN=${WIN:-22}
UD=${UD:-target/title-evidence/menu-prof-sw/userdata}
LOG=${LOG:-target/title-evidence/menu-prof-sw/run.log}
INPUT=${INPUT:-target/title-evidence/menu-prof-sw/input.txt}
FR=${FR:-target/title-evidence/menu-prof-sw/frames}
PPM_DIR=${PPM_DIR:-target/title-evidence/menu-prof-sw/ppm}
mkdir -p "$(dirname "$OUT")" "$UD" "$FR" "$PPM_DIR"; mkdir -p "$(dirname "$LOG")"
rm -f "$OUT" "$OUT".* ; : > "$LOG"; printf '0\n' > "$INPUT"
command -v callgrind_control >/dev/null || { echo "no callgrind_control"; exit 2; }

env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_SKIP_MOVIE=1 \
    DQ8_USER_DATA_DIR="$UD" DQ8_INPUT_FILE="$INPUT" \
    DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$PPM_DIR" \
  valgrind --tool=callgrind --callgrind-out-file="$OUT" --instr-atstart=no \
    --cache-sim=no --branch-sim=no \
    "$BIN" DQ8.code target/dq8.romfs >/dev/null 2>"$LOG" &
PID=$!
echo "pid=$PID pre=$PRE win=$WIN"
# Dismiss dialog + walk to the dimmed main-menu title: A presses at 8,16,24s.
# NOPRESS=1 skips them to profile the stable save-data dialog instead.
if [[ -z "${NOPRESS:-}" ]]; then
for p in 8 16 24; do
  sleep $(( p - ${prev:-0} )); prev=$p
  printf '1\n' > "$INPUT"; sleep 1; printf '0\n' > "$INPUT"
  echo "A press @~${p}s"
done
fi
# Ensure PRE total elapsed before instrumentation on.
_pv=${prev:-0}
_sl=$(( PRE - _pv )); (( _sl < 1 )) && _sl=1
sleep "$_sl"
cp -f "$PPM_DIR"/dq8-screen.ppm "$FR/prof_scene.ppm" 2>/dev/null || true
echo "== collection ON =="
callgrind_control -i on >/dev/null 2>&1
sleep "$WIN"
echo "== dump =="
callgrind_control -d >/dev/null 2>&1
callgrind_control -i off >/dev/null 2>&1
kill "$PID" 2>/dev/null; sleep 2; kill -9 "$PID" 2>/dev/null
PROFILE=$(ls -1 "$OUT" "$OUT".* 2>/dev/null | sort | tail -1)
[[ -f ${PROFILE:-} ]] || { echo "no profile written"; exit 1; }
echo "profile: $PROFILE ($(stat -c%s "$PROFILE") bytes)"
echo "=== top functions (self cost %) ==="
callgrind_annotate --threshold=90 "$PROFILE" 2>/dev/null | grep -A80 "PROGRAM TOTALS\|^ *[0-9,]*.*\.[ch]" | head -70
