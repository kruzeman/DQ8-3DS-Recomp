#!/usr/bin/env bash
# t_6b81015d: deterministic fixed-guest-budget A/B (DQ8_BENCH_STEPS), GL backend.
# usage: t6b_bench_ab.sh <binary> <label>
set -u
BIN=$1; LAB=$2; OUT=/tmp/t6/ab/$LAB; UD=/tmp/t6/ab/ud_$LAB
W=/home/hermes/DQ8/worktrees/rendermenu-t_6b81015d
mkdir -p "$OUT" "$UD"; rm -f "$OUT"/*.ppm
cd "$W"
env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_GL=1 DQ8_BENCH_STEPS=20000000 \
    DQ8_BENCH_NO_SNAPSHOT=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT" \
    DQ8_USER_DATA_DIR="$UD" "$BIN" DQ8.code dq8.romfs \
    --load-state /home/hermes/DQ8/saves/dq8-state.sav > "$OUT/run.log" 2>&1 || true
echo "$LAB frames=$(ls "$OUT"/*.ppm 2>/dev/null | wc -l) md5=$(md5sum "$OUT"/dq8-screen.ppm 2>/dev/null | cut -d' ' -f1) $(grep -F 'BENCH steps=' "$OUT/run.log" | tail -1)"
