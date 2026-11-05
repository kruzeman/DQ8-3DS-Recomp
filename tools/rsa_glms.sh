#!/usr/bin/env bash
# t_05281b89 stage A: interleaved gl_ms measurement.
# Runs the SAME deterministic scenario at scale 1 / 2 / 4, three interleaved
# repetitions each (1,2,4,1,2,4,1,2,4), so a drift in machine load cannot
# favour one scale.  Then prints the FRAMEPROF statistics per run.
#   tools/rsa_glms.sh <outroot> <savestate> [steps] [reps]
set -u
OUT=${1:?outdir}; STATE=${2:?savestate}; STEPS=${3:-8000000}; REPS=${4:-3}
W=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=${BIN:-target/dq8-rsa4}
mkdir -p "$OUT"
for r in $(seq 1 "$REPS"); do
  for s in 1 2 4; do
    d="$OUT/rep$r-x$s"
    mkdir -p "$d"
    "$W/tools/rsa_run.sh" "$s" "$d" "$STEPS" "$BIN" "$STATE" > "$d/stdout.txt" 2>&1
    printf 'rep%s x%s: %s\n' "$r" "$s" "$(grep -m1 'coverage (draws)' "$d/run.log" | sed 's/.*(draws): //')"
  done
done
echo
echo "== gl_ms per frame (all runs, interleaved)"
python3 "$W/tools/rsa_glms.py" $(for r in $(seq 1 "$REPS"); do for s in 1 2 4; do echo -n "rep$r-x$s=$OUT/rep$r-x$s/run.log "; done; done)
