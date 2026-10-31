#!/usr/bin/env bash
# reach_name_entry.sh (t_86b8f1f9) — drive from fresh userdata through the
# first save dialog into the character-name/StreetPass dialog with scripted
# DQ8_INPUT_FILE presses, capturing screen PPMs + GL texture-path stats.
#
# usage: tools/reach_name_entry.sh <binary> <run-name> [total-secs]
#   env: NE_GL=1 (default on), NE_PRESS_AT="160 168 176 184 ..." (secs)
#
# Evidence under target/name-entry-evidence/<run-name>/:
#   screen-*.ppm   throttled screen snapshots (5/s cap in the runtime)
#   tr-*.ppm       every guest display transfer (frame checkpoints)
#   run.log        full runtime log (FRAMESTAT/texstat/GL lines)
#   input.log      when each press happened
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT=$PWD
BIN=${1:?binary}
RUN=${2:?run-name}
SECS=${3:-300}
OUT=$ROOT/target/name-entry-evidence/$RUN
case $BIN in /*) ;; *) BIN=$ROOT/$BIN ;; esac
[[ -x $BIN ]] || { echo "no binary $BIN" >&2; exit 2; }
[[ -e $OUT ]] && { echo "evidence dir exists: $OUT" >&2; exit 2; }
mkdir -p "$OUT/userdata" "$OUT/screens"

printf '0' > "$OUT/input"
: > "$OUT/run.log"
: > "$OUT/input.log"

env_common=(
  DQ8_RUN_GUEST_THREADS=1
  DQ8_HEADLESS=1
  DQ8_USER_DATA_DIR=$OUT/userdata
  DQ8_INPUT_FILE=$OUT/input
  DQ8_FRAME_STATS=1
  DQ8_GL_TEXSTATS=1
  DQ8_CAPTURE_PPM=1
  DQ8_CAPTURE_PPM_DIR=$OUT/screens
  DQ8_DUMP_TRANSFER_PREFIX=$OUT/tr
)
if [[ "${NE_GL:-1}" != "0" ]]; then
  env_common+=(DQ8_GL=1)
fi

T0=$(date +%s)
"${env_common[@]}" "$BIN" /home/hermes/DQ8/DQ8.code /home/hermes/DQ8/target/dq8.romfs \
  >> "$OUT/run.log" 2>&1 &
GAME=$!

press() {
  local now=$(( $(date +%s) - T0 ))
  printf '1' > "$OUT/input"
  echo "t=${now}s press" >> "$OUT/input.log"
  sleep 1
  printf '0' > "$OUT/input"
  echo "t=$(( $(date +%s) - T0 ))s release" >> "$OUT/input.log"
}

# Press schedule: default advances past the save dialog into the naming flow.
SCHEDULE=${NE_PRESS_AT:-"162 170 178 186 194 202 210 220 230 240"}
next_press=0
read -r -a presses <<< "$SCHEDULE"
while kill -0 "$GAME" 2>/dev/null; do
  now=$(( $(date +%s) - T0 ))
  (( now >= SECS )) && break
  if (( next_press < ${#presses[@]} )) && (( now >= presses[next_press] )); then
    press
    next_press=$(( next_press + 1 ))
  fi
  sleep 1
done
printf '0' > "$OUT/input"
kill -TERM "$GAME" 2>/dev/null
sleep 2
kill -KILL "$GAME" 2>/dev/null
wait "$GAME" 2>/dev/null
echo "--- run finished at t=$(( $(date +%s) - T0 ))s ---"
echo "evidence: $OUT"
echo "screens: $(ls "$OUT/screens" 2>/dev/null | wc -l) ppm, transfers: $(ls "$OUT"/tr-*.ppm 2>/dev/null | wc -l)"
grep -E "FRAMESTAT totals|GL texture|GL context hold|GL backend: shutdown" "$OUT/run.log" | tail -8
