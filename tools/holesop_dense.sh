#!/usr/bin/env bash
# t_a22413f1: dense (10 Hz) capture from the operator savestate so GL and SW
# runs actually land on the same guest frame and can be compared BYTE EXACTLY.
# usage: holesop_dense.sh <outdir> <gl 0|1> [secs]
set -u
OUT=${1:?outdir}; GL=${2:?0|1}; SECS=${3:-20}
W=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=${BIN:-$W/target/dq8-holesop-final}
STATE=${STATE:-/home/hermes/DQ8/saves/dq8-state.sav}
mkdir -p "$OUT" "$W/.ud_smoke2"
rm -f "$OUT"/s*.ppm "$OUT/run.log" "$OUT/index.tsv"
cd "$W" || exit 1
env -u DISPLAY DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT" \
    DQ8_GL="$GL" DQ8_RUN_GUEST_THREADS=1 DQ8_ELIDE_RASTER=0 \
    DQ8_USER_DATA_DIR="$W/.ud_smoke2" \
    "$BIN" DQ8.code dq8.romfs --load-state "$STATE" > "$OUT/run.log" 2>&1 &
GP=$!
N=$((SECS * 10))
for ((i = 1; i <= N; i++)); do
    sleep 0.1
    kill -0 $GP 2>/dev/null || break
    S="$OUT/dq8-screen.ppm"
    if [ -f "$S" ]; then
        sz=$(stat -c %s "$S" 2>/dev/null || echo 0)
        if [ "$sz" -ge 288015 ]; then
            md=$(md5sum "$S" | cut -d' ' -f1)
            cp -f "$S" "$OUT/s$(printf '%05d' $i).ppm" 2>/dev/null || true
            echo "s$i md5=$md" >> "$OUT/index.tsv"
        fi
    fi
done
kill -TERM $GP 2>/dev/null; sleep 2; kill -KILL $GP 2>/dev/null; wait $GP 2>/dev/null
echo "done $OUT gl=$GL samples=$(wc -l < "$OUT/index.tsv")"
