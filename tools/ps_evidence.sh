#!/usr/bin/env bash
# t_fdd26eba evidence: display transfer at scale (A) + present the scaled screen (B).
#
#   bash tools/ps_evidence.sh [outdir]
#
# Deterministic 8M-step headless llvmpipe runs (the RX 580 was offline for this
# card — every timing here is software GL, not a GPU number).
#
#   A  x1 identity vs the base binary (dq8-b49 head) : DET digests + shadow PPMs
#      + the composed window vs a reference composed from the base's own dumps
#   B  x2/x4 scaled vs guest in field, battle, menu : mean|d|, best shift,
#      2x2-block uniformity, guest-path byte identity of the bottom rect
#   C  cost: 3 interleaved reps of x1/x2/x4 in field and battle
set -u
W=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$W"
BIN=${BIN:-target/dq8-ps}
BASE=${BASE:-target/dq8-base}
OUT=${1:-/tmp/ps_ev}
FIELD=/home/hermes/DQ8/evidence/render-cape-path/dq8-state.sav
BATTLE=/home/hermes/DQ8/evidence/hot-regions-20261102/state-battle.sav
MENU=/home/hermes/DQ8/evidence/hot-regions-20261102/state-menu.sav
mkdir -p "$OUT" "$OUT/ud"

run() { # name scale source state
    local n=$1 s=$2 src=$3 st=$4
    mkdir -p "$OUT/$n/win"
    env DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT/$n" \
        DQ8_PRESENT_DUMP_PPM="$OUT/$n/win" \
        DQ8_RUN_GUEST_THREADS=1 DQ8_USER_DATA_DIR="$OUT/ud" DQ8_GL=1 \
        DQ8_RENDER_SCALE="$s" DQ8_PRESENT_SOURCE="$src" \
        DQ8_DETERMINISTIC=1 DQ8_BENCH_STEPS=8000000 DQ8_FRAMEPROF=1 \
        "$BIN" DQ8.code dq8.romfs --load-state "$st" > "$OUT/$n/run.log" 2>&1
    local det gl pr
    det=$(grep -o 'DET_FRAME md5=[0-9a-f]*' "$OUT/$n/run.log" | head -1)
    gl=$(awk '/^FRAMEPROF /{for(i=1;i<=NF;i++) if ($i ~ /^gl_ms=/){split($i,a,"="); s+=a[2]}} END{printf "%.1f", s+0}' "$OUT/$n/run.log")
    pr=$(awk '/^FRAMEPROF /{for(i=1;i<=NF;i++) if ($i ~ /^present_ms=/){split($i,a,"="); s+=a[2]}} END{printf "%.1f", s+0}' "$OUT/$n/run.log")
    echo "[$n] $det gl_ms=$gl present_ms=$pr $(grep -o 'display transfer at scale: [0-9]* taken' "$OUT/$n/run.log" | tail -1) $(grep -o 'transfers taken=[0-9]* store_fail=[0-9]* screen_uses=[0-9]*/[0-9]* absent_fallbacks=[0-9]*/[0-9]* stale_drops=[0-9]*' "$OUT/$n/run.log" | tail -1)"
}

echo "=== A: x1 identity vs the base binary (field) ==="
env DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT/base_f1" \
    DQ8_RUN_GUEST_THREADS=1 DQ8_USER_DATA_DIR="$OUT/ud" DQ8_GL=1 \
    DQ8_RENDER_SCALE=1 DQ8_DETERMINISTIC=1 DQ8_BENCH_STEPS=8000000 DQ8_FRAMEPROF=1 \
    "$BASE" DQ8.code dq8.romfs --load-state "$FIELD" > "$OUT/base_f1.log" 2>&1
mkdir -p "$OUT/base_f1"
run x1_guest 1 guest "$FIELD"
run x1_scaled 1 scaled "$FIELD"
cmp -s "$OUT/base_f1/dq8-screen.ppm" "$OUT/x1_guest/dq8-screen.ppm" && echo "x1 top shadow: IDENTICAL to base" || echo "x1 top shadow: DIFFERS"
cmp -s "$OUT/base_f1/dq8-screen-bottom.ppm" "$OUT/x1_guest/dq8-screen-bottom.ppm" && echo "x1 bottom shadow: IDENTICAL to base" || echo "x1 bottom shadow: DIFFERS"
python3 tools/ps_ref_window.py "$OUT/base_f1/dq8-screen.ppm" "$OUT/base_f1/dq8-screen-bottom.ppm" "$OUT/ref_window.ppm"
cmp -s "$OUT/ref_window.ppm" "$OUT/x1_guest/win/dq8-present.ppm" && echo "x1 window: IDENTICAL to base-composed reference" || echo "x1 window: DIFFERS from reference"
cmp -s "$OUT/x1_guest/win/dq8-present.ppm" "$OUT/x1_scaled/win/dq8-present.ppm" && echo "x1 DQ8_PRESENT_SOURCE=scaled: IDENTICAL to guest" || echo "x1 scaled: DIFFERS"

echo "=== B: scaled vs guest, three scenes ==="
for pair in "field:$FIELD" "battle:$BATTLE" "menu:$MENU"; do
    sc=${pair%%:*}; st=${pair#*:}
    for f in 2 4; do
        run "${sc}_x${f}_guest" "$f" guest "$st"
        run "${sc}_x${f}_scaled" "$f" scaled "$st"
    done
done

echo "=== C: cost, 3 interleaved reps, field + battle ==="
for rep in 1 2 3; do
    for f in 1 2 4; do
        run "cost_field_x${f}_r${rep}" "$f" scaled "$FIELD"
        run "cost_battle_x${f}_r${rep}" "$f" scaled "$BATTLE"
    done
done
echo "done: $OUT"
