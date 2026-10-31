#!/usr/bin/env bash
# t_432a85ab: profile run — binary + per-thread CPU/wchan sampler.
# usage: profile_run.sh <label> <duration_s> [extra_env...]
set -u
WT=/home/hermes/DQ8/worktrees/texcache2-t_66c89cf2
BIN=$WT/target/dq8-texcache3
CODE=/home/hermes/DQ8/DQ8.code
ROMFS=/home/hermes/DQ8/target/dq8.romfs
STATE=/home/hermes/DQ8/saves/dq8-state.sav
HONEY=/home/hermes/DQ8/saves/honeySave.bin
WS=/home/hermes/.hermes/kanban/workspaces/t_432a85ab
LAB=${1:-prof}
DUR=${2:-70}
shift 2 || true
EXTRA=("$@")

OUT=$WS/evidence/$LAB
mkdir -p "$OUT/frames" "$OUT/userdata/archive-00000004-00000000"
cp -f "$HONEY" "$OUT/userdata/archive-00000004-00000000/honeySave.bin"
printf '0\n' > "$OUT/input.txt"
: > "$OUT/run.log"

cd "$WT" || exit 1
env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
    DQ8_PROGRESS_SECONDS=10 \
    DQ8_FRAME_STATS=1 DQ8_TRACE_TIME=1 \
    DQ8_GL=1 \
    "${EXTRA[@]}" \
    DQ8_USER_DATA_DIR="$OUT/userdata" DQ8_INPUT_FILE="$OUT/input.txt" \
    "$BIN" "$CODE" "$ROMFS" --load-state "$STATE" > "$OUT/run.log" 2>&1 &
gp=$!
echo "pid=$gp out=$OUT dur=$DUR start=$(date +%s.%N)" > "$OUT/timeline.log"

# let it warm up (savestate load ~1 s + first texture uploads)
sleep 8
bash "$WS/tsample.sh" "$gp" 0.1 "$((DUR-8))" "$OUT/threads.log" &
sp=$!
echo "sampler_pid=$sp" >> "$OUT/timeline.log"

sleep "$((DUR-8))"
kill -TERM "$gp" 2>/dev/null; sleep 3; kill -9 "$gp" 2>/dev/null
wait "$gp" 2>/dev/null
kill "$sp" 2>/dev/null; wait "$sp" 2>/dev/null
echo "end=$(date +%s.%N)" >> "$OUT/timeline.log"
echo "OUT=$OUT"
grep -c . "$OUT/threads.log"
grep -c "FRAMESTAT f=" "$OUT/run.log"
