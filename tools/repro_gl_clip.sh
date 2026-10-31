#!/usr/bin/env bash
# Repro: fresh launch save-check dialog, GL vs software frame capture.
# usage: repro_gl_clip.sh <label> <gl|sw>
set -uo pipefail
LABEL=${1:?label}
MODE=${2:?gl|sw}
BIN=/home/hermes/DQ8/worktrees/gl-clip-t_5b283f2a/target/dq8-gl-clip
CODE=/home/hermes/DQ8/DQ8.code
ROMFS=/home/hermes/DQ8/target/dq8.romfs
WT=/home/hermes/DQ8/worktrees/gl-clip-t_5b283f2a
OUT="$WT/evidence/$LABEL"
rm -rf "$OUT"; mkdir -p "$OUT/userdata" "$OUT/frames"
INPUT="$OUT/input.txt"; printf '0\n' > "$INPUT"
if [[ "$MODE" == "gl" ]]; then export DQ8_GL=1; else export DQ8_GL=0; fi
cd "$WT"
DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_SKIP_MOVIE=1 \
DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT/frames" \
DQ8_TRACE_INPUT=1 DQ8_INPUT_FILE="$INPUT" \
DQ8_USER_DATA_DIR="$OUT/userdata" \
"$BIN" "$CODE" "$ROMFS" > "$OUT/run.log" 2>&1 &
GP=$!
# capture for ~40s, enough to reach + sit on the save dialog
for i in $(seq 1 20); do
  sleep 2
  kill -0 $GP 2>/dev/null || break
done
kill -9 $GP 2>/dev/null
sleep 1
echo "=== $LABEL ($MODE) frames ==="
ls -la "$OUT/frames/" | tail -20
