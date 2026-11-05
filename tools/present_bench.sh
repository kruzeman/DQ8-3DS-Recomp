#!/usr/bin/env bash
# tools/present_bench.sh -- field-state present-path A/B (t_5286064e).
#
# Runs the SAME savestate through several binary/knob arms, interleaved
# (arm1 run1, arm2 run1, ... arm1 run2, ...), on a private Xvfb display so the
# X11 SHM present path is exercised, and keeps every log.  The guest, the
# state, the build flags and the harness line are identical between arms, so
# the only difference is the presentation path.
#
# usage:
#   tools/present_bench.sh OUTDIR SECS RUNS "label:binary[:ENV=VAL;ENV=VAL]" ...
#
# env (defaults match the operator's field benchmark):
#   DQ8_STATE   savestate  (default /home/hermes/DQ8/evidence/rx580-holes-20261101/scene-holes.sav)
#   DQ8_DISPLAY_SIZE  Xvfb geometry (default 800x960x24)
#   DQ8_LOCK=0  skip tools/lock_run.sh (only for smoke tests)
#
# Each arm writes $OUT/<label>-r<n>.log (stderr: FRAMEPROF/FPSLOG/TEMPO) and
# .out (stdout).  Summarise with tools/analyze_present.py.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${1:?usage: present_bench.sh OUTDIR SECS RUNS arm...}
SECS=${2:?}
RUNS=${3:?}
shift 3
STATE=${DQ8_STATE:-/home/hermes/DQ8/evidence/rx580-holes-20261101/scene-holes.sav}
GEOM=${DQ8_DISPLAY_SIZE:-800x960x24}
mkdir -p "$OUT"

# Private X display: the X11 SHM present path needs a real X server, and a
# private display keeps the run off the operator's desktop.
DISP=:97
if ! xdpyinfo -display "$DISP" >/dev/null 2>&1; then
  Xvfb "$DISP" -screen 0 "$GEOM" -nolisten tcp > "$OUT/xvfb.log" 2>&1 &
  XVFB_PID=$!
  for _ in $(seq 50); do
    xdpyinfo -display "$DISP" >/dev/null 2>&1 && break
    sleep 0.1
  done
  trap 'kill $XVFB_PID 2>/dev/null' EXIT
fi

BASE=(
  "DISPLAY=$DISP"
  DQ8_GL=1 DQ8_GL_TEXCACHE=1 DQ8_RUN_GUEST_THREADS=1 DQ8_VSPAR=4 DQ8_SPLIT_GL=0
  DQ8_STACK_NO_RELOCATE=1 DQ8_GL_DEPTH_ATTACH=1
  DQ8_FRAMEPROF=1 DQ8_FPSLOG=1 DQ8_TEMPO=1 DQ8_PRESENT_STATS=1
)

run_arm() {  # label binary extra-env run
  local label=$1 bin=$2 extra=$3 run=$4
  local -a arm_env=()
  local kv
  IFS=';' read -r -a pairs <<< "$extra"
  for kv in "${pairs[@]}"; do
    [[ -n "$kv" ]] && arm_env+=("$kv")
  done
  local ud="$OUT/ud-$label"
  mkdir -p "$ud"
  echo "== $label run $run ($bin) ${arm_env[*]:-}"
  if [[ "${DQ8_LOCK:-1}" == "0" ]]; then
    env "${BASE[@]}" "${arm_env[@]}" DQ8_USER_DATA_DIR="$ud" \
      timeout -k 5 "$SECS" "$bin" --load-state "$STATE" DQ8.code dq8.romfs \
      > "$OUT/$label-r$run.out" 2> "$OUT/$label-r$run.log"
  else
    env "${BASE[@]}" "${arm_env[@]}" DQ8_USER_DATA_DIR="$ud" \
      tools/lock_run.sh --kind measure -- \
      timeout -k 5 "$SECS" "$bin" --load-state "$STATE" DQ8.code dq8.romfs \
      > "$OUT/$label-r$run.out" 2> "$OUT/$label-r$run.log"
  fi
  local rc=$?
  grep -c '^FRAMEPROF ' "$OUT/$label-r$run.log" | sed "s/^/   frameprof_lines=/"
  tail -1 "$OUT/$label-r$run.log" | cut -c1-160
  return $rc
}

for ((r = 1; r <= RUNS; r++)); do
  for arm in "$@"; do
    label=${arm%%:*}
    rest=${arm#*:}
    bin=${rest%%:*}
    extra=""
    [[ "$rest" == *:* ]] && extra=${rest#*:}
    run_arm "$label" "$bin" "$extra" "$r"
  done
done
echo "logs in $OUT"
