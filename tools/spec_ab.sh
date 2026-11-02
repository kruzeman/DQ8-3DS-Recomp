#!/usr/bin/env bash
# t_5ddc492f: paired A/B of the fragment-shader specialisation.
#
#   tools/spec_ab.sh <outdir> <binary> [pairs] [SECS]
#
# Alternates two runs of the operator savestate scene:
#   DQ8_GL_SPEC=0  generic shader (pre-specialisation reference)
#   DQ8_GL_SPEC=1  generated per-shape shaders
# so host-load drift hits both sides.  Each run is a fresh process with its own
# user-data dir; the scene animation is not deterministic across runs, so only
# per-counter means are compared (never images).
set -u
OUT=${1:?outdir}; BIN=${2:?binary}; PAIRS=${3:-2}; SECS=${4:-20}
W=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
STATE=${STATE:-/home/hermes/DQ8/saves/dq8-state.sav}
EXTRA=("${@:5}")
mkdir -p "$OUT"
unset DQ8_SAVESTATE_AUTOSAVE_AT
cd "$W"
run() { # run <tag> <specval>
  local tag=$1 spec=$2 ud="$OUT/ud_$1"
  local -a ev=(DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_TIME=1
               DQ8_FRAMEPROF=1 "DQ8_GL_SPEC=$spec" "DQ8_USER_DATA_DIR=$ud")
  mkdir -p "$ud"
  if (( ${#EXTRA[@]} )); then ev+=("${EXTRA[@]}"); fi
  env "${ev[@]}" "$BIN" DQ8.code dq8.romfs --load-state "$STATE" \
      > "$OUT/$tag.log" 2>&1 &
  local gp=$!
  sleep "$SECS"
  kill -9 $gp 2>/dev/null
  wait $gp 2>/dev/null
  echo "done $tag"
}
for p in $(seq 1 "$PAIRS"); do
  run "p${p}_spec0" 0
  run "p${p}_spec1" 1
done
python3 tools/spec_ab.py "$OUT"
