#!/usr/bin/env bash
# t_dd830a74 round 3 evidence: the shared-stager placement fix for item 2.
#
# Every run is DQ8_DETERMINISTIC=1, 8M steps, headless llvmpipe (the RX 580 was
# offline for this card; every timing here is software GL).
#
#   bash tools/dt_evidence_r3.sh [outdir]
#
# Sections:
#   A  x1 in all three scenes           -> must equal the dq8-b49 digests
#   B  x4 in all three, sampled ON      -> the new default (item 2)
#   C  x4 in all three, DQ8_RS_SAMPLE_SCALED=0 -> must equal stage A byte for byte
#   D  cost: 3 interleaved reps of x1/x2/x4 in field and battle
#   E  dumps used by dt_snap.py / dt_stage.py / cmp_prepost.py
set -u
W=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$W"
BIN=${BIN:-target/dq8-dt1}
OUT=${OUT:-/tmp/dt_r3}
FIELD=/home/hermes/DQ8/evidence/render-cape-path/dq8-state.sav
BATTLE=/home/hermes/DQ8/evidence/hot-regions-20261102/state-battle.sav
MENU=/home/hermes/DQ8/evidence/hot-regions-20261102/state-menu.sav
mkdir -p "$OUT" "$OUT/ud"

run() { # name scale state extra
    local n=$1 s=$2 st=$3 ex=${4:-}
    mkdir -p "$OUT/$n"
    env DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT/$n" \
        DQ8_RUN_GUEST_THREADS=1 DQ8_USER_DATA_DIR="$OUT/ud" DQ8_GL=1 \
        DQ8_RENDER_SCALE="$s" DQ8_DETERMINISTIC=1 DQ8_BENCH_STEPS=8000000 \
        DQ8_FRAMEPROF=1 $ex \
        "$BIN" DQ8.code dq8.romfs --load-state "$st" > "$OUT/$n/run.log" 2>&1
    local gl
    gl=$(awk '/^FRAMEPROF /{for(i=1;i<=NF;i++) if ($i ~ /^gl_ms=/){split($i,a,"="); s+=a[2]}} END{printf "%.1f", s+0}' "$OUT/$n/run.log")
    echo "[$n] $(grep -o 'DET_FRAME md5=[0-9a-f]*' "$OUT/$n/run.log" | head -1)  gl_ms_sum=$gl"
    echo "[$n] $(grep -o 'scanout_1F300000 md5=[0-9a-f]*' "$OUT/$n/run.log" | head -1) $(grep -o 'scanout_1F346500 md5=[0-9a-f]*' "$OUT/$n/run.log" | head -1)"
    echo "[$n] $(grep -o 'coverage (draws): scaled [0-9]*, native [0-9]*' "$OUT/$n/run.log" | tail -1)"
    echo "[$n] $(grep -o 'scaled samples): unit-0 samples of the current target served from the scaled backing [0-9]*, fallbacks to the native staged image [0-9]*' "$OUT/$n/run.log" | tail -1)"
    echo "[$n] $(grep -o 'placement — [^(]*' "$OUT/$n/run.log" | head -1)"
    echo "[$n] $(grep -o 'download boundary[^:]*: [0-9]* flushes, [0-9.]* ms total, [0-9]* us each' "$OUT/$n/run.log" | head -1)"
}

echo "=== A: x1, all scenes (must equal dq8-b49) ==="
run f1 1 "$FIELD"
run b1 1 "$BATTLE"
run m1 1 "$MENU"
echo "=== B: x4, sampled backing ON (default) ==="
run f4 4 "$FIELD"
run b4 4 "$BATTLE"
run m4 4 "$MENU"
echo "=== C: x4, DQ8_RS_SAMPLE_SCALED=0 (must equal stage A) ==="
run f4n 4 "$FIELD" "DQ8_RS_SAMPLE_SCALED=0"
run b4n 4 "$BATTLE" "DQ8_RS_SAMPLE_SCALED=0"
run m4n 4 "$MENU" "DQ8_RS_SAMPLE_SCALED=0"
echo "=== D: cost, 3 interleaved reps ==="
for rep in 1 2 3; do
    for s in 1 2 4; do
        run "cf${s}_r${rep}" "$s" "$FIELD"
        run "cb${s}_r${rep}" "$s" "$BATTLE"
    done
done
echo "=== E: dumps (snapshot trio + native staged trio) ==="
mkdir -p "$OUT/dump_snap" "$OUT/dump_nat"
run f4dump 4 "$FIELD" "DQ8_RS_SNAP_DUMP=$OUT/dump_snap"
run f4dnat 4 "$FIELD" "DQ8_RS_SAMPLE_SCALED=0 DQ8_RS_SNAP_DUMP=$OUT/dump_nat"
echo "done: $OUT"
