#!/usr/bin/env bash
# t_d559a510: A/B perf of the fixed-function blend path vs the shader blend
# path at the operator's savestate.  Alternating runs (so machine load drifts
# hit both arms equally), no input, no draw-hash readback.
#   usage: tools/glb_perf.sh [reps] [secs]
set -u
W=/home/hermes/DQ8/worktrees/glblend-t_d559a510
BIN=${BIN:-$W/target/dq8-glblend}
REPS=${1:-3}
SECS=${2:-20}
OUT=${OUT:-$W/evidence/t_d559a510/a3/perf}
UD=/home/hermes/.hermes/profiles/coder/cache/scratch/glb_ud_perf
mkdir -p "$OUT" "$UD"
cd "$W"
one() { # name blendsecs
  local name=$1 val=$2 d="$OUT/$1_$3"
  mkdir -p "$d"
  env DQ8_HEADLESS=1 DQ8_TRACE_TIME=1 DQ8_RUN_GUEST_THREADS=1 \
      DQ8_USER_DATA_DIR="$UD" DQ8_GL=1 DQ8_GL_BLENDS="$val" \
      "$BIN" DQ8.code dq8.romfs --load-state /home/hermes/DQ8/saves/dq8-state.sav \
      > "$d/run.log" 2>&1 &
  local gp=$!
  sleep "$SECS"
  kill -9 $gp 2>/dev/null; wait $gp 2>/dev/null
  echo "$name run=$3 $(grep -c GLPHASE "$d/run.log") phases"
}

for r in $(seq 1 "$REPS"); do
  one gl 1 "$r"
  one sw 0 "$r"
done
python3 "$W/tools/glb_perf.py" "$OUT"
