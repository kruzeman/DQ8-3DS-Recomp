#!/usr/bin/env bash
# t_074669f7: savestate fast-start probe.  Loads the operator's dq8-state.sav
# ("oi guv!" forest scene) directly — no 15-minute choreography — and captures
# top-screen PPM snapshots + (optionally) per-draw DRAW lines.
#
# usage: BW_BIN=target/dq8-terrain BW_OUT=terrain-base \
#          BW_STATE=/home/hermes/DQ8/saves/dq8-state.sav \
#          BW_DUR=60 BW_SNAP=5 BW_EXTRA="DQ8_TRACE_LAYER=1" \
#          bash tools/run_state_probe.sh
set -u
WT=${BW_WT:-$(cd "$(dirname "$0")/.." && pwd)}
BIN=${BW_BIN:-target/dq8-terrain}
PREFIX=${BW_OUT:-state-probe}
DUR=${BW_DUR:-60}
SNAP=${BW_SNAP:-5}
STATE=${BW_STATE:-/home/hermes/DQ8/saves/dq8-state.sav}
HONEY=${BW_HONEY:-/home/hermes/DQ8/saves/honeySave.bin}
GL=${BW_GL:-1}
CODE=${BW_CODE:-/home/hermes/DQ8/DQ8.code}
ROMFS=${BW_ROMFS:-/home/hermes/DQ8/target/dq8.romfs}

OUT=$WT/target/blackworld-evidence/$PREFIX
mkdir -p "$OUT/frames"
rm -rf "$OUT/userdata"; mkdir -p "$OUT/userdata/archive-00000004-00000000"
cp -f "$HONEY" "$OUT/userdata/archive-00000004-00000000/honeySave.bin"
printf '0\n' > "$OUT/input.txt"
: > "$OUT/run.log"; : > "$OUT/timeline.log"

EXTRA_ENV=()
[[ -n "${BW_EXTRA:-}" ]] && read -ra EXTRA_ENV <<< "$BW_EXTRA"
GLARG=0; [ "$GL" = 1 ] && GLARG=1

echo "t0=$(date +%s.%N) bin=$BIN gl=$GLARG state=$STATE dur=$DUR snap=$SNAP extra=${BW_EXTRA:-}" >> "$OUT/timeline.log"
env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
    DQ8_PROGRESS_SECONDS=10 DQ8_CAPTURE_PPM=1 \
    DQ8_CAPTURE_PPM_DIR="$OUT/frames" \
    DQ8_FRAME_STATS=1 DQ8_TRACE_GSP_FB=1 \
    DQ8_GL=$GLARG "${EXTRA_ENV[@]}" \
    DQ8_USER_DATA_DIR="$OUT/userdata" DQ8_INPUT_FILE="$OUT/input.txt" \
    "$WT/$BIN" "$CODE" "$ROMFS" --load-state "$STATE" > "$OUT/run.log" 2>&1 &
gp=$!

n=0
for ((t=1; t<=DUR; t++)); do
  sleep 1
  if ! kill -0 "$gp" 2>/dev/null; then
    echo "t=$t process exited early" >> "$OUT/timeline.log"; break
  fi
  if (( t % SNAP == 0 )); then
    n=$((n+1))
    [ -f "$OUT/frames/dq8-screen.ppm" ] && cp -f "$OUT/frames/dq8-screen.ppm" "$OUT/frames/t$(printf '%04d' $t).ppm"
    [ -f "$OUT/frames/dq8-screen-bottom.ppm" ] && cp -f "$OUT/frames/dq8-screen-bottom.ppm" "$OUT/frames/t$(printf '%04d' $t).bottom.ppm"
    nz=$(grep -c . "$OUT/frames/t$(printf '%04d' $t).ppm" 2>/dev/null || echo 0)
    echo "t=$t snap$n" >> "$OUT/timeline.log"
  fi
done
kill "$gp" 2>/dev/null
for _ in 1 2 3 4 5; do kill -0 "$gp" 2>/dev/null || break; sleep 1; done
kill -9 "$gp" 2>/dev/null
wait "$gp" 2>/dev/null
echo "done t_end=$(date +%s.%N)" >> "$OUT/timeline.log"
echo "OUT=$OUT"
