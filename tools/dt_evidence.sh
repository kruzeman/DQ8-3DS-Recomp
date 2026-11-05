#!/usr/bin/env bash
# t_dd830a74 evidence battery: gate-removal correctness + cost + the opt-in
# scaled-sample experiment.  Headless llvmpipe (no dGPU on this host).
set -u
W=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$W"
BIN=${BIN:-target/dq8-dt1}
OUT=${OUT:-/tmp/dt3}
FIELD=/home/hermes/DQ8/evidence/render-cape-path/dq8-state.sav
BATTLE=/home/hermes/DQ8/evidence/hot-regions-20261102/state-battle.sav
MENU=/home/hermes/DQ8/evidence/hot-regions-20261102/state-menu.sav
mkdir -p "$OUT"
run() { # name scale state extra
    local n=$1 s=$2 st=$3 ex=${4:-}
    mkdir -p "$OUT/$n"
    env DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT/$n" \
        DQ8_RUN_GUEST_THREADS=1 DQ8_USER_DATA_DIR="$OUT/ud" DQ8_GL=1 \
        DQ8_RENDER_SCALE="$s" DQ8_DETERMINISTIC=1 DQ8_BENCH_STEPS=8000000 \
        DQ8_FRAMEPROF=1 $ex \
        "$BIN" DQ8.code dq8.romfs --load-state "$st" > "$OUT/$n/run.log" 2>&1
    echo "[$n] $(grep -m1 'DET_FRAME' "$OUT/$n/run.log")"
    echo "[$n] $(grep -o 'scanout_1F300000 md5=[0-9a-f]*' "$OUT/$n/run.log")"
    echo "[$n] $(grep -o 'retired=[0-9]* audio_frames=[0-9]*' "$OUT/$n/run.log" | head -1)"
    echo "[$n] $(grep -o 'targets forced native [0-9]*' "$OUT/$n/run.log" | tail -1) $(grep -o 'scaled backing [0-9]*, fallbacks[^,]*[0-9]*' "$OUT/$n/run.log" | tail -1)"
    grep -o 'render scale coverage (draws): scaled [0-9]*, native [0-9]*' "$OUT/$n/run.log" | tail -1 | sed "s/^/[$n] /"
    grep -o '8x8 cells last drawn at the scaled size ([0-9.]*%)' "$OUT/$n/run.log" | tail -1 | sed "s/^/[$n] /"
    if [ -f "$OUT/$n/dq8-screen.ppm" ]; then
        echo "[$n] ppm md5 $(md5sum "$OUT/$n/dq8-screen.ppm" | cut -d' ' -f1)"
    fi
}
echo "=== correctness ==="
run f1 1 "$FIELD"
run f2 2 "$FIELD"
run f4 4 "$FIELD"
run b1 1 "$BATTLE"
run b4 4 "$BATTLE"
run m1 1 "$MENU"
run m4 4 "$MENU"
echo "=== opt-in scaled sample (experimental) ==="
run f4s 4 "$FIELD" "DQ8_RS_SAMPLE_SCALED=1"
run f4s_gl 4 "$FIELD" "DQ8_RS_SAMPLE_SCALED=1 DQ8_GL_DEBUG=1"
echo "=== cost: 3 interleaved runs per scale ==="
for rep in 1 2 3; do
    for s in 1 2 4; do
        run "cf${s}_r${rep}" "$s" "$FIELD"
        run "cb${s}_r${rep}" "$s" "$BATTLE"
    done
done
