#!/usr/bin/env bash
# t_3de68379: clean (untraced) savestate-scene run for the fps / frame-time
# number of one stage.  usage: tools/codegen_clean_run.sh <label> <binary> [dur_s]
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
LAB=${1:?label}
BIN=${2:?binary}
DUR=${3:-150}
case "$BIN" in /*) ;; *) BIN="$PWD/$BIN" ;; esac
OUT="$PWD/evidence/t_3de68379/$LAB"
mkdir -p "$OUT/userdata/archive-00000004-00000000"
cp -f /home/hermes/DQ8/saves/honeySave.bin "$OUT/userdata/archive-00000004-00000000/honeySave.bin"
printf '0\n' > "$OUT/input.txt"
: > "$OUT/run.log"
env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
    DQ8_FRAME_STATS=1 DQ8_TRACE_TIME=1 DQ8_GL=1 \
    DQ8_USER_DATA_DIR="$OUT/userdata" DQ8_INPUT_FILE="$OUT/input.txt" \
    "$BIN" /home/hermes/DQ8/DQ8.code /home/hermes/DQ8/target/dq8.romfs \
    --load-state /home/hermes/DQ8/saves/dq8-state.sav > "$OUT/run.log" 2>&1 &
gp=$!
sleep "$DUR"
kill -TERM "$gp" 2>/dev/null || true
sleep 3
kill -9 "$gp" 2>/dev/null || true
wait "$gp" 2>/dev/null || true
echo "== $LAB ($(basename "$BIN")) $DUR s =="
python3 tools/framebudget/extract_stats.py "$OUT/run.log"
