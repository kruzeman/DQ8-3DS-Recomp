#!/usr/bin/env bash
# Headless integration smoke for the host->HID input path (PERF-05 change).
#
# Runs the real split binary for ~36 s with DQ8_TRACE_INPUT and drives the host
# button mask through DQ8_INPUT_FILE (the non-SDL host input source), so the
# production publication path is exercised end to end: file -> native_hid_buttons
# -> HID ring entry/edge -> guest-visible shared memory + event signalling.
# SDL is not involved here (headless); the SDL ordering itself is covered by
# tools/test_hid_publish_order.py.
#
# usage: tools/smoke_hid_input.sh [binary] [log]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT=$PWD
BIN=${1:-target/dq8-startup-split}
LOG=${2:-target/smoke-hid-input.log}
SECS=${DQ8_SMOKE_SECS:-120}
STATE=${DQ8_SMOKE_DIR:-$ROOT/target/smoke-hid-input}
case $STATE in /*) ;; *) STATE=$ROOT/$STATE ;; esac
mkdir -p "$STATE/userdata"
printf '0' > "$STATE/input"
: > "$LOG"
DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
  DQ8_INPUT_FILE=$STATE/input DQ8_USER_DATA_DIR=$STATE/userdata \
  timeout -s KILL "$SECS" "$BIN" DQ8.code dq8.romfs >> "$LOG" 2>&1 &
pid=$!
# Press/release A four times over the run (the file is re-read by every due pump).
for _ in 1 2 3 4; do
  sleep 10
  printf '1' > "$STATE/input"
  sleep 10
  printf '0' > "$STATE/input"
done
wait "$pid" 2>/dev/null
echo "input-file=$STATE/input userdata=$STATE/userdata"
echo "hid-published-edges: $(grep -c 'HID buttons=' "$LOG")"
grep 'HID buttons=' "$LOG" | head -8
echo "--- tail ---"
tail -3 "$LOG"
