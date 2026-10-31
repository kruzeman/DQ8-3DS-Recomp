#!/usr/bin/env bash
# t_3de68379: STATIC-MASK scene gate for one stage binary (operator hint,
# same method as tools/tc3_static_mask.py from 7edd466).
#
# The 3D scene is animated: characters idle-move, so two runs - and even two
# moments of one run - are never full-frame byte identical.  This script
# captures TWO frames of the SAME run (t=$1 and t=$2 seconds of wall time)
# with the runtime's throttled DQ8_CAPTURE_PPM writer; the STATIC MASK is the
# set of pixels byte-identical between them, i.e. terrain / UI / static props.
#
# usage: tools/cg_static_gate.sh <binary> <label> <t1_s> <t2_s> <dur_s>
# writes evidence/t_3de68379/static/<label>/{a1.ppm,a2.ppm,run.log}
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

BIN=${1:?binary}
LAB=${2:?label}
T1=${3:-20}
T2=${4:-50}
DUR=${5:-70}
case "$BIN" in /*) ;; *) BIN="$PWD/$BIN" ;; esac
OUT="$PWD/evidence/t_3de68379/static/$LAB"
mkdir -p "$OUT/userdata/archive-00000004-00000000"
rm -f "$OUT/a1.ppm" "$OUT/a2.ppm" "$OUT/dq8-screen.ppm" "$OUT/run.log"
cp -f /home/hermes/DQ8/saves/honeySave.bin \
      "$OUT/userdata/archive-00000004-00000000/honeySave.bin"
printf '0\n' > "$OUT/input.txt"
: > "$OUT/run.log"

env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
    DQ8_FRAME_STATS=1 DQ8_TRACE_TIME=1 DQ8_GL=1 \
    DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT" \
    DQ8_USER_DATA_DIR="$OUT/userdata" DQ8_INPUT_FILE="$OUT/input.txt" \
    "$BIN" /home/hermes/DQ8/DQ8.code /home/hermes/DQ8/target/dq8.romfs \
    --load-state /home/hermes/DQ8/saves/dq8-state.sav > "$OUT/run.log" 2>&1 &
gp=$!
sleep "$T1"
[ -f "$OUT/dq8-screen.ppm" ] && cp -f "$OUT/dq8-screen.ppm" "$OUT/a1.ppm"
sleep $(( T2 - T1 ))
[ -f "$OUT/dq8-screen.ppm" ] && cp -f "$OUT/dq8-screen.ppm" "$OUT/a2.ppm"
sleep 2
kill -TERM "$gp" 2>/dev/null || true
sleep 3
kill -9 "$gp" 2>/dev/null || true
wait "$gp" 2>/dev/null || true
echo "OUT=$OUT a1=$(stat -c %s "$OUT/a1.ppm" 2>/dev/null || echo missing) a2=$(stat -c %s "$OUT/a2.ppm" 2>/dev/null || echo missing)"
grep -o 'cum: frames=[0-9]*' "$OUT/run.log" | tail -1
