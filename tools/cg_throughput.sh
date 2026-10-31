#!/usr/bin/env bash
# t_3de68379: guest-throughput micro-A/B - retire a fixed guest-instruction
# budget (DQ8_BENCH_STEPS) and report the wall time it took.  This is the
# direct measure of the thing the codegen change modifies (dispatch cost per
# guest instruction) and is far less sensitive to the box's co-tenant load
# than fps, which is dominated by llvmpipe rasterisation.
#
# usage: tools/cg_throughput.sh <binary> <label> [steps]
# prints one BENCH line; returns non-zero if the run did not reach it.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
BIN=${1:?binary}
LAB=${2:?label}
STEPS=${3:-30000000}
case "$BIN" in /*) ;; *) BIN="$PWD/$BIN" ;; esac
OUT="$PWD/evidence/t_3de68379/tp/$LAB"
mkdir -p "$OUT/userdata/archive-00000004-00000000"
cp -f /home/hermes/DQ8/saves/honeySave.bin \
      "$OUT/userdata/archive-00000004-00000000/honeySave.bin"
printf '0\n' > "$OUT/input.txt"
: > "$OUT/run.log"
start=$(date +%s.%N)
env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
    DQ8_FRAME_STATS=0 DQ8_TRACE_TIME=0 DQ8_GL=1 \
    DQ8_BENCH_STEPS="$STEPS" DQ8_BENCH_NO_SNAPSHOT=1 \
    DQ8_USER_DATA_DIR="$OUT/userdata" DQ8_INPUT_FILE="$OUT/input.txt" \
    "$BIN" /home/hermes/DQ8/DQ8.code /home/hermes/DQ8/target/dq8.romfs \
    --load-state /home/hermes/DQ8/saves/dq8-state.sav > "$OUT/run.log" 2>&1 || true
end=$(date +%s.%N)
bench=$(grep -F 'BENCH steps=' "$OUT/run.log" | tail -1 || true)
wall=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.3f", b - a }')
echo "$LAB wall=${wall}s ${bench:-NO_BENCH_LINE}"
