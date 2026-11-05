#!/usr/bin/env bash
# vspar_ab.sh -- one measured run of the vertex-pool card (t_f40374bc).
#
#   tools/vspar_ab.sh <binary> <scene> <tag> [secs] [VAR=value...]
#
# Runs the runtime headless on ~/hotreg/states/<scene>.sav at x1, samples
# whole-process and per-thread CPU over the run (tools/vspar_probe.py) and
# prints the fps / guest_ms summary (tools/vspar_sum.py).  Every setting that
# differs from the baseline is passed as a trailing VAR=value, so a sweep is
# just a loop over arguments.
set -u
BIN=$(realpath "${1:?binary}"); SCENE=${2:?scene}; TAG=${3:?tag}
SECS=${4:-20}; shift 4 2>/dev/null || shift $#
HOT=${HOT:-$HOME/hotreg}
OUT=${VS_OUT:-$HOT/vs-out}
UD=$HOT/ud-vs-$TAG
mkdir -p "$OUT" "$UD"
cd "$HOME/DQ8" || exit 2

# The rig is shared with other cards: wait for a quiet window (no other
# emulator run) before measuring, so CPU shares and fps are not contaminated.
deadline=$((SECONDS + ${VS_WAIT:-180}))
while pgrep -u "$USER" -f 'dq8[^ ]* --load-state' >/dev/null 2>&1; do
  if (( SECONDS >= deadline )); then
    echo "WARN: rig still busy after ${VS_WAIT:-180}s -- measuring anyway"
    break
  fi
  sleep 2
done

# Optional CPU pinning for the 8-core-laptop emulation: VS_PIN=0-7.
LAUNCH=()
[ -n "${VS_PIN:-}" ] && LAUNCH=(taskset -c "$VS_PIN")

env DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_SPLIT_GL=0 \
    DQ8_FN_EMIT=all DQ8_FRAMEPROF=1 DQ8_FPSLOG=1 DQ8_VBLANK_WALLCLOCK=1 \
    DQ8_TEMPO=1 DQ8_RENDER_SCALE=1 DQ8_TRACE_TIME=1 \
    DQ8_USER_DATA_DIR="$UD" "$@" \
  python3 "$HOT/vspar_probe.py" --tag "$TAG" --secs "$SECS" --settle 4 \
    --out "$OUT" -- \
    "${LAUNCH[@]}" "$BIN" --load-state "$HOT/states/$SCENE.sav" DQ8.code dq8.romfs
python3 "$HOT/vspar_sum.py" "$OUT/$TAG.err" "$TAG"
grep -m1 '^VSPAR ' "$OUT/$TAG.err" | sed 's/^/  /'
