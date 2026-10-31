#!/usr/bin/env bash
export DQ8_CAPTURE_PPM=1   # PPM screen snapshots are opt-in since audit 5.4
# Traced headless run of the DQ8 recompilation up to the save/extra-data
# creation stage, for DSP/CSND/IPC audio discovery.
# usage: tools/trace_audio_stage.sh <label> <duration_s> [press_second ...]
set -u
cd /home/hermes/DQ8/worktrees/audio || exit 1

BIN=${BIN:-/home/hermes/DQ8/target/dq8-startup-split}
CODE=/home/hermes/DQ8/DQ8.code
ROMFS=/home/hermes/DQ8/target/dq8.romfs

LABEL=${1:?label}
DUR=${2:?duration}
shift 2
PRESSES=("$@")

OUT=target/audio-evidence/$LABEL
FRAMES=$OUT/frames
INPUT=$OUT/input.txt
UD=target/audio-ud-$LABEL
PPM_FULL=288015

if [ ! -x "$BIN" ]; then echo "missing binary $BIN" >&2; exit 1; fi
mkdir -p "$FRAMES" "$UD"
printf '0\n' > "$INPUT"
rm -rf /tmp/dq8-screen-audio.ppm 2>/dev/null || true
: > "$OUT/run.log"
: > "$OUT/timeline.log"

snapshot() {
    local i
    for i in 1 2 3 4 5; do
        if [ -f /tmp/dq8-screen.ppm ]; then
            cp -f /tmp/dq8-screen.ppm "$FRAMES/$1.ppm" 2>/dev/null || true
            local size
            size=$(stat -c %s "$FRAMES/$1.ppm" 2>/dev/null || echo 0)
            if [ "$size" -ge "$PPM_FULL" ]; then return 0; fi
        fi
        sleep 0.2
    done
    rm -f "$FRAMES/$1.ppm"
    return 1
}

echo "t0=$(date +%s.%N) pid=? label=$LABEL duration=$DUR presses=${PRESSES[*]:-none}" >> "$OUT/timeline.log"

DQ8_HEADLESS=1 DQ8_DISPLAY=0 DQ8_RUN_GUEST_THREADS=1 \
DQ8_TRACE_IPC=1 DQ8_TRACE_INPUT=1 DQ8_PROGRESS_SECONDS=1 \
DQ8_USER_DATA_DIR="$PWD/$UD" DQ8_INPUT_FILE="$PWD/$INPUT" \
"$BIN" "$CODE" "$ROMFS" > "$OUT/run.log" 2>&1 &
GP=$!
echo "pid=$GP" >> "$OUT/timeline.log"
sed -i "s/pid=?/pid=$GP/" "$OUT/timeline.log"

alive=1
for ((t = 1; t <= DUR; t++)); do
    sleep 1
    if ! kill -0 "$GP" 2>/dev/null; then
        echo "t=$t process exited early" >> "$OUT/timeline.log"
        alive=0
        break
    fi
    if (( t % 5 == 0 )); then snapshot "t$(printf '%04d' "$t")"; fi
    for p in ${PRESSES[@]+${PRESSES[@]}}; do
        if [ "$p" = "$t" ]; then
            echo "t=$t $(date +%s.%N) A_DOWN (1000ms)" >> "$OUT/timeline.log"
            printf '1\n' > "$INPUT"
            snapshot "t$(printf '%04d' "$t")_press_down"
            sleep 1
            printf '0\n' > "$INPUT"
            echo "t=$t $(date +%s.%N) A_UP" >> "$OUT/timeline.log"
            snapshot "t$(printf '%04d' "$t")_press_up"
        fi
    done
done
if kill -0 "$GP" 2>/dev/null; then
    kill -TERM "$GP" 2>/dev/null
    sleep 3
    kill -KILL "$GP" 2>/dev/null
    wait "$GP" 2>/dev/null
fi
echo "done=$(date +%s.%N)" >> "$OUT/timeline.log"
echo "log lines: $(wc -l < "$OUT/run.log")"
