#!/usr/bin/env bash
# t_fdd26eba evidence runner: one deterministic headless run per (scene, scale,
# present source) with the composed-window dump on, so the acceptance numbers
# (2x2-block share, best-shift mean|d|, framing) can be measured on the image
# the window would show.
#
#   bash tools/ps_run.sh <outdir> <name> <scale> <source> <savestate> [extra env]
#
# Everything is llvmpipe here (the RX 580 was offline for this card), so no
# number from this script is a GPU claim.
set -u
W=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$W"
BIN=${BIN:-target/dq8-ps}
OUT=${1:?outdir}
NAME=${2:?name}
SCALE=${3:?scale}
SRC=${4:?source}
STATE=${5:?savestate}
EXTRA=${6:-}
mkdir -p "$OUT/$NAME/win" "$OUT/ud"
env DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT/$NAME" \
    DQ8_PRESENT_DUMP_PPM="$OUT/$NAME/win" \
    DQ8_RUN_GUEST_THREADS=1 DQ8_USER_DATA_DIR="$OUT/ud" DQ8_GL=1 \
    DQ8_RENDER_SCALE="$SCALE" DQ8_PRESENT_SOURCE="$SRC" \
    DQ8_DETERMINISTIC=1 DQ8_BENCH_STEPS="${STEPS:-8000000}" DQ8_FRAMEPROF=1 $EXTRA \
    "$BIN" DQ8.code dq8.romfs --load-state "$STATE" > "$OUT/$NAME/run.log" 2>&1
echo "[$NAME] rc=$? $(grep -o 'DET_FRAME md5=[0-9a-f]*' "$OUT/$NAME/run.log" | head -1)"
echo "[$NAME] $(grep -o 'scanout_1F300000 md5=[0-9a-f]*' "$OUT/$NAME/run.log" | head -1) $(grep -o 'scanout_1F346500 md5=[0-9a-f]*' "$OUT/$NAME/run.log" | head -1)"
echo "[$NAME] $(grep -o 'display transfer at scale: .*' "$OUT/$NAME/run.log" | tail -1)"
echo "[$NAME] $(grep -o 'present scaled source: transfers taken=.*' "$OUT/$NAME/run.log" | tail -1)"
echo "[$NAME] windows=$(ls "$OUT/$NAME/win" 2>/dev/null | wc -l)"
