#!/usr/bin/env bash
export DQ8_CAPTURE_PPM=1   # PPM screen snapshots are opt-in since audit 5.4
# Y2R cinematic harness: headless run, optional A presses, PPM frame capture.
#
# usage: tools/run_y2r.sh <label> <userdata_dir> <duration_s> [press_second ...]
#
#   label        evidence directory becomes target/y2r-evidence/<label>
#   userdata_dir DQ8_USER_DATA_DIR (use a COPY of a valid save; never the original)
#   duration_s   wall-clock seconds to keep the game running
#   press_second seconds (from start) at which a 1000 ms A press is injected
#
# Env:
#   Y2R_BIN   binary to run        (default ./target/dq8-startup-split)
#   Y2R_EXTRA extra env assignments (e.g. 'DQ8_TRACE_IPC=1 DQ8_TRACE_WAIT=1')
set -u
cd /home/hermes/DQ8 || exit 1

LABEL=${1:?label}
UD=${2:?userdata dir}
DUR=${3:?duration}
shift 3
PRESSES=("$@")

BIN=${Y2R_BIN:-./target/dq8-startup-split}
OUT=target/y2r-evidence/$LABEL
FRAMES=$OUT/frames
INPUT=$OUT/input.txt
PPM_FULL=288015   # P6\n400 240\n255\n + 400*240*3

if [ ! -x "$BIN" ]; then echo "missing binary $BIN" >&2; exit 1; fi
mkdir -p "$FRAMES"
printf '0\n' > "$INPUT"
rm -f /tmp/dq8-screen.ppm
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

# shellcheck disable=SC2086
env DQ8_HEADLESS=1 DQ8_DISPLAY=0 DQ8_RUN_GUEST_THREADS=1 \
    DQ8_TRACE_IPC=1 DQ8_TRACE_INPUT=1 DQ8_PROGRESS_SECONDS=1 \
    DQ8_USER_DATA_DIR="$UD" DQ8_INPUT_FILE="$INPUT" $Y2R_EXTRA \
    "$BIN" DQ8.code target/dq8.romfs > "$OUT/run.log" 2>&1 &
GP=$!
echo "t0=$(date +%s.%N) pid=$GP label=$LABEL userdata=$UD duration=$DUR presses=${PRESSES[*]:-none} extra=${Y2R_EXTRA:-none}" \
    >> "$OUT/timeline.log"

alive=1
for ((t = 1; t <= DUR; t++)); do
    sleep 1
    if ! kill -0 "$GP" 2>/dev/null; then
        echo "t=$t process exited early" >> "$OUT/timeline.log"
        alive=0
        break
    fi
    if (( t % 5 == 0 )); then snapshot "t$(printf '%04d' "$t")"; fi
    for p in ${PRESSES[@]+"${PRESSES[@]}"}; do
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

snapshot "t$(printf '%04d' "$DUR")_final"
if [ "$alive" = 1 ] && kill -0 "$GP" 2>/dev/null; then
    echo "t=$DUR $(date +%s.%N) SIGTERM (clean shutdown)" >> "$OUT/timeline.log"
    kill -TERM "$GP"
fi
wait "$GP"
echo "exit=$? t_end=$(date +%s.%N)" >> "$OUT/timeline.log"

echo "--- $LABEL: frames=$(ls "$FRAMES" | wc -l)"
echo "--- $LABEL: escapes/errors:"
grep -cE "translated PC escaped|unsupported|segmentation" "$OUT/run.log"
