#!/usr/bin/env bash
export DQ8_CAPTURE_PPM=1   # PPM screen snapshots are opt-in since audit 5.4
# comb2 = comb1 + GSP display-transfer accounting (no rebuild needed):
#   * DQ8_DUMP_TRANSFER_PREFIX+LATEST dumps every guest top/bottom transfer
#     AND prints one "GSP display-transfer dump" log line per transfer -> the
#     per-second line count in timeline.log tells whether the guest keeps
#     presenting during the black window (t~60..140 in comb1) or stalls.
#   * dense frame snapshots (2 s) around the press + save stage.
#   * DQ8_TRACE_IPC for the explicit FS FormatSaveData evidence line.
set -u
cd /home/hermes/DQ8/worktrees/audio || exit 1

BIN=${BIN:-$PWD/target/dq8-startup-split-combined}
CODE=/home/hermes/DQ8/DQ8.code
ROMFS=/home/hermes/DQ8/target/dq8.romfs

LABEL=${1:?label}
DUR=${2:?duration}
PRESS=${3:?press second}
PRESS2=${4:-0}

OUT=target/audio-evidence/$LABEL
FRAMES=$OUT/frames
INPUT=$OUT/input.txt
UD=target/audio-ud-$LABEL
PPM_FULL=288015

if [ ! -x "$BIN" ]; then echo "missing binary $BIN" >&2; exit 1; fi
rm -rf "$UD" "$OUT"; mkdir -p "$FRAMES" "$UD" "$OUT/se"
printf '0\n' > "$INPUT"
rm -f /tmp/dq8-screen.ppm /tmp/dq8-screen-bottom.ppm 2>/dev/null || true
: > "$OUT/run.log"
echo "t0=$(date +%s)" > "$OUT/timeline.log"

snapshot() {
    local i size
    for i in 1 2 3 4 5 6 7 8 9 10; do
        if [ -f /tmp/dq8-screen.ppm ]; then
            cp -f /tmp/dq8-screen.ppm "$FRAMES/$1.ppm" 2>/dev/null || true
            size=$(stat -c %s "$FRAMES/$1.ppm" 2>/dev/null || echo 0)
            if [ "$size" -ge "$PPM_FULL" ]; then break; fi
        fi
        sleep 0.2
    done
    if [ -f /tmp/dq8-screen-bottom.ppm ]; then
        cp -f /tmp/dq8-screen-bottom.ppm "$FRAMES/$1.bot.ppm" 2>/dev/null || true
    fi
    return 0
}

XVFB_DISP=":95"
Xvfb "$XVFB_DISP" -screen 0 1024x768x24 -ac -nolisten tcp >/dev/null 2>&1 &
XP=$!
sleep 1
if ! kill -0 "$XP" 2>/dev/null; then echo "Xvfb failed" >&2; exit 1; fi
trap 'kill $XP 2>/dev/null' EXIT

DISPLAY=$XVFB_DISP \
DQ8_DISPLAY=1 DQ8_RUN_GUEST_THREADS=1 \
DQ8_TRACE_INPUT=1 DQ8_PROGRESS_SECONDS=1 DQ8_TRACE_THREADS=1 DQ8_TRACE_AUDIO=1 \
DQ8_TRACE_FS=1 DQ8_TRACE_IPC=1 \
DQ8_TRACE_PC="${DQ8_TRACE_PC_OVERRIDE:-0x00472804,0x004727FC}" \
DQ8_DUMP_TRANSFER_PREFIX="$PWD/$OUT/tr" DQ8_DUMP_TRANSFER_LATEST=1 \
DQ8_DSP_DUMP="$PWD/$OUT/dsp" \
DQ8_DSP_SE_DIR="$PWD/$OUT/se" \
DQ8_AUDIO_DUMP="$PWD/$OUT/audio.wav" \
SDL_AUDIODRIVER=dummy \
DQ8_USER_DATA_DIR="$PWD/$UD" DQ8_INPUT_FILE="$PWD/$INPUT" \
"$BIN" "$CODE" "$ROMFS" > "$OUT/run.log" 2>&1 &
GP=$!
echo "pid=$GP" >> "$OUT/timeline.log"

last_xfer=0
alive=1
for ((t = 1; t <= DUR; t++)); do
    sleep 1
    if ! kill -0 "$GP" 2>/dev/null; then
        echo "t=$t process exited early" >> "$OUT/timeline.log"
        alive=0
        break
    fi
    # count of guest display transfers so far (1 line each)
    n=$(grep -c 'GSP display-transfer' "$OUT/run.log" 2>/dev/null || echo 0)
    echo "t=$t xfer=$n delta=$((n - last_xfer))" >> "$OUT/timeline.log"
    last_xfer=$n
    if (( t % 5 == 0 )); then snapshot "t$(printf '%04d' "$t")"; fi
    # dense snapshots across the dialog/save window
    if (( t >= PRESS - 5 && t <= PRESS + 60 && t % 2 == 0 )); then
        snapshot "dense_$(printf '%04d' "$t")"
    fi
    if (( t == PRESS2 )); then
        echo "t=$t $(date +%s) A2_DOWN (1000ms)" >> "$OUT/timeline.log"
        printf '1\n' > "$INPUT"
        sleep 1
        printf '0\n' > "$INPUT"
        echo "t=$t $(date +%s) A2_UP" >> "$OUT/timeline.log"
    fi
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
echo "xfer total: $(grep -c 'GSP display-transfer' "$OUT/run.log" || true)"
echo "$LABEL complete"
