#!/usr/bin/env bash
# Windowed-path smoke with the SDL dummy video driver.
#
# Unlike tools/smoke_hid_input.sh (DQ8_HEADLESS=1, no SDL at all), this run
# brings up the real SDL display path under SDL_VIDEODRIVER=dummy, so
# host_display_init/present and the reordered host_display_pump_events loop run
# in the production binary.  Host buttons again come from DQ8_INPUT_FILE,
# because SDL events cannot be injected into another process.
#
# usage: tools/smoke_hid_input_sdl.sh [binary]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT=$PWD
BIN=${1:-target/dq8-startup-split}
STATE=${DQ8_SMOKE_DIR:-$ROOT/target/smoke-hid-input-sdl}
case $STATE in /*) ;; *) STATE=$ROOT/$STATE ;; esac
mkdir -p "$STATE/userdata"
printf '0' > "$STATE/input"
: > "$STATE/run.log"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
DQ8_INPUT_FILE=$STATE/input DQ8_USER_DATA_DIR=$STATE/userdata \
  timeout -s KILL "${DQ8_SMOKE_SECS:-60}" "$BIN" DQ8.code dq8.romfs >> "$STATE/run.log" 2>&1 &
pid=$!
sleep 25
printf '1' > "$STATE/input"
sleep 15
printf '0' > "$STATE/input"
wait "$pid" 2>/dev/null
echo "log=$STATE/run.log"
grep -E "host display|HID handles|HID buttons=|quit requested" "$STATE/run.log" | head -14
