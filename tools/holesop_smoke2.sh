#!/usr/bin/env bash
# t_a22413f1: capture K frames/second-aligned screenshots from the operator
# savestate for one backend, so GL/SW differences can be separated from
# run-to-run (guest phase) variance.  usage: holesop_smoke2.sh <outdir> <gl 0|1> [secs]
set -u
OUT=${1:?outdir}; GL=${2:?0|1}; SECS=${3:-25}
W=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=${BIN:-$W/target/dq8-holesop-final}
STATE=${STATE:-/home/hermes/DQ8/saves/dq8-state.sav}
mkdir -p "$OUT" "$W/.ud_smoke2"
rm -f "$OUT"/t*.ppm "$OUT/run.log"
cd "$W" || exit 1
env -u DISPLAY DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT" \
    DQ8_GL="$GL" DQ8_RUN_GUEST_THREADS=1 DQ8_ELIDE_RASTER=0 DQ8_FRAME_STATS=1 \
    DQ8_USER_DATA_DIR="$W/.ud_smoke2" \
    "$BIN" DQ8.code dq8.romfs --load-state "$STATE" > "$OUT/run.log" 2>&1 &
GP=$!
for ((t = 1; t <= SECS; t++)); do
    sleep 1
    kill -0 $GP 2>/dev/null || { echo "t=$t exited" >> "$OUT/frames.tsv"; break; }
    S="$OUT/dq8-screen.ppm"
    if [ -f "$S" ]; then
        md=$(md5sum "$S" | cut -d' ' -f1)
        cp -f "$S" "$OUT/t$(printf '%04d' $t).ppm" 2>/dev/null || true
        echo "t=$t md5=$md" >> "$OUT/frames.tsv"
    fi
done
kill -TERM $GP 2>/dev/null; sleep 2; kill -KILL $GP 2>/dev/null; wait $GP 2>/dev/null
echo "done $OUT gl=$GL secs=$SECS"
