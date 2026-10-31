#!/usr/bin/env bash
export DQ8_CAPTURE_PPM=1   # PPM screen snapshots are opt-in since audit 5.4
# comb1 = full operator scenario on the COMBINED stand (test/combined-stage):
# windowed (Xvfb), DQ8_RUN_GUEST_THREADS=1, fresh userdata, WAV dump,
# frame snapshots every 5 s + around the A press.
# Usage: run_combined1.sh <label> <duration_s> <press_second>
set -u
cd /home/hermes/DQ8/worktrees/audio || exit 1

BIN=${BIN:-$PWD/target/dq8-startup-split-combined}
CODE=/home/hermes/DQ8/DQ8.code
ROMFS=/home/hermes/DQ8/target/dq8.romfs

LABEL=${1:?label}
DUR=${2:?duration}
PRESS=${3:?press second}

OUT=target/audio-evidence/$LABEL
FRAMES=$OUT/frames
INPUT=$OUT/input.txt
UD=target/audio-ud-$LABEL
PPM_FULL=288015

if [ ! -x "$BIN" ]; then echo "missing binary $BIN" >&2; exit 1; fi
rm -rf "$UD" "$OUT"; mkdir -p "$FRAMES" "$UD" "$OUT/se"
printf '0\n' > "$INPUT"
rm -f /tmp/dq8-screen.ppm 2>/dev/null || true
: > "$OUT/run.log"
echo "t0=$(date +%s)" > "$OUT/timeline.log"

snapshot() {
    local i size
    for i in 1 2 3 4 5 6 7 8 9 10; do
        if [ -f /tmp/dq8-screen.ppm ]; then
            cp -f /tmp/dq8-screen.ppm "$FRAMES/$1.ppm" 2>/dev/null || true
            size=$(stat -c %s "$FRAMES/$1.ppm" 2>/dev/null || echo 0)
            if [ "$size" -ge "$PPM_FULL" ]; then return 0; fi
        fi
        sleep 0.2
    done
    rm -f "$FRAMES/$1.ppm"
    return 1
}

XVFB_DISP=":96"
Xvfb "$XVFB_DISP" -screen 0 1024x768x24 -ac -nolisten tcp >/dev/null 2>&1 &
XP=$!
sleep 1
if ! kill -0 "$XP" 2>/dev/null; then echo "Xvfb failed" >&2; exit 1; fi
trap 'kill $XP 2>/dev/null' EXIT

echo "t0s=$(date +%s) label=$LABEL combined windowed duration=$DUR press=$PRESS xvfb=$XVFB_DISP" >> "$OUT/timeline.log"

DISPLAY=$XVFB_DISP \
DQ8_DISPLAY=1 DQ8_RUN_GUEST_THREADS=1 \
DQ8_TRACE_INPUT=1 DQ8_PROGRESS_SECONDS=1 DQ8_TRACE_THREADS=1 DQ8_TRACE_AUDIO=1 \
DQ8_TRACE_FS=1 \
DQ8_TRACE_PC="${DQ8_TRACE_PC_OVERRIDE:-0x00472804,0x004727FC}" \
DQ8_DSP_DUMP="$PWD/$OUT/dsp" \
DQ8_DSP_SE_DIR="$PWD/$OUT/se" \
DQ8_AUDIO_DUMP="$PWD/$OUT/audio.wav" \
SDL_AUDIODRIVER=dummy \
DQ8_USER_DATA_DIR="$PWD/$UD" DQ8_INPUT_FILE="$PWD/$INPUT" \
"$BIN" "$CODE" "$ROMFS" > "$OUT/run.log" 2>&1 &
GP=$!
echo "pid=$GP" >> "$OUT/timeline.log"

alive=1
for ((t = 1; t <= DUR; t++)); do
    sleep 1
    if ! kill -0 "$GP" 2>/dev/null; then
        echo "t=$t process exited early" >> "$OUT/timeline.log"
        alive=0
        break
    fi
    if (( t % 5 == 0 )); then snapshot "t$(printf '%04d' "$t")"; fi
    if (( t == PRESS )); then
        echo "t=$t $(date +%s) A_DOWN (1000ms)" >> "$OUT/timeline.log"
        printf '1\n' > "$INPUT"
        snapshot "t$(printf '%04d' "$t")_press_down"
        sleep 1
        printf '0\n' > "$INPUT"
        echo "t=$t $(date +%s) A_UP" >> "$OUT/timeline.log"
        snapshot "t$(printf '%04d' "$t")_press_up"
    fi
done
if kill -0 "$GP" 2>/dev/null; then
    snapshot "t9999_final"
    kill -TERM "$GP" 2>/dev/null; sleep 3; kill -KILL "$GP" 2>/dev/null; wait "$GP" 2>/dev/null
fi
wait "$GP" 2>/dev/null
echo "done=$(date +%s)" >> "$OUT/timeline.log"
echo "escapes: $(grep -c 'translated PC escaped' "$OUT/run.log" || true)"
echo "$LABEL complete"
