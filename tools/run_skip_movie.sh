#!/usr/bin/env bash
# DQ8_SKIP_MOVIE verification harness: boot -> logos -> save-creation dialogs
# (A-press juggling) -> intro movie -> state after the intro (main menu).
#
# DQ8_SKIP_MOVIE=1 skips the intro video: the runtime drives the game's own
# movie-end state transition at startup so the guest lands on the main menu
# without decoding/playing the intro.  Without the flag the intro plays exactly
# as before (this harness is the before/after comparison).  Always run with
# DQ8_RUN_GUEST_THREADS=1 (defaulted here, as in tools/run_static_recomp.sh).
#
# usage: tools/run_skip_movie.sh <label> <userdata_dir> <duration_s> [press_second ...]
#
#   label         evidence dir becomes target/skip-evidence/<label>
#   userdata_dir  DQ8_USER_DATA_DIR; pass a NEW empty dir for the fresh-userdata
#                 (dialog + save creation) scenario, or an existing save dir
#   duration_s    wall-clock seconds to keep the guest running
#   press_second  second at which a 1000 ms A press is injected (dialog juggling)
#
# Environment:
#   SKIP_MOVIE=1  sets DQ8_SKIP_MOVIE=1 for this run (the flag under test)
#   SKIP_BIN      binary to run (default target/dq8-main)
#   SKIP_EXTRA    extra env assignments (e.g. 'DQ8_TRACE_Y2R=1 DQ8_TRACE_PC=0x002FA318')
set -u
cd "$(dirname "$0")/.." || exit 1

LABEL=${1:?label}
UD=${2:?userdata dir}
DUR=${3:?duration}
shift 3
PRESSES=("$@")

BIN=${SKIP_BIN:-target/dq8-main}
OUT=target/skip-evidence/$LABEL
FRAMES=$OUT/frames
INPUT=$OUT/input.txt
PPM_FULL=288015   # "P6\n400 240\n255\n" + 400*240*3
PPM_BOT_FULL=23055

if [ ! -x "$BIN" ]; then echo "missing binary $BIN" >&2; exit 1; fi
mkdir -p "$FRAMES" "$UD"
printf '0\n' > "$INPUT"
rm -f /tmp/dq8-screen.ppm /tmp/dq8-screen-bottom.ppm
: > "$OUT/run.log"
: > "$OUT/timeline.log"

snapshot() {   # snapshot <name>
    local i size
    for i in 1 2 3 4 5; do
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
snapshot_bottom() {   # snapshot_bottom <name>
    local size
    if [ -f /tmp/dq8-screen-bottom.ppm ]; then
        size=$(stat -c %s /tmp/dq8-screen-bottom.ppm 2>/dev/null || echo 0)
        if [ "$size" -ge "$PPM_BOT_FULL" ]; then
            cp -f /tmp/dq8-screen-bottom.ppm "$FRAMES/$1.bottom.ppm" 2>/dev/null || true
        fi
    fi
}

SKIP_ENV=()
if [ "${SKIP_MOVIE:-0}" = "1" ]; then SKIP_ENV+=(DQ8_SKIP_MOVIE=1); fi

# shellcheck disable=SC2086
env DQ8_HEADLESS=1 DQ8_DISPLAY=0 DQ8_RUN_GUEST_THREADS=1 \
    DQ8_TRACE_INPUT=1 DQ8_PROGRESS_SECONDS=1 \
    DQ8_USER_DATA_DIR="$PWD/$UD" DQ8_INPUT_FILE="$PWD/$INPUT" \
    ${SKIP_ENV[@]+"${SKIP_ENV[@]}"} ${SKIP_EXTRA:-} \
    "$BIN" DQ8.code target/dq8.romfs > "$OUT/run.log" 2>&1 &
GP=$!
echo "t0=$(date +%s.%N) pid=$GP label=$LABEL bin=$BIN userdata=$UD duration=$DUR presses=${PRESSES[*]:-none} skip_movie=${SKIP_MOVIE:-0} extra=${SKIP_EXTRA:-none}" \
    >> "$OUT/timeline.log"

alive=1
for ((t = 1; t <= DUR; t++)); do
    sleep 1
    if ! kill -0 "$GP" 2>/dev/null; then
        echo "t=$t process exited early" >> "$OUT/timeline.log"
        alive=0
        break
    fi
    for p in ${PRESSES[@]+"${PRESSES[@]}"}; do
        if [ "$p" = "$t" ]; then
            echo "t=$t $(date +%s.%N) A_DOWN (1000ms)" >> "$OUT/timeline.log"
            printf '1\n' > "$INPUT"
            snapshot "t$(printf '%04d' "$t")_press_down"
            snapshot_bottom "t$(printf '%04d' "$t")_press_down"
            sleep 1
            printf '0\n' > "$INPUT"
            echo "t=$t $(date +%s.%N) A_UP" >> "$OUT/timeline.log"
            snapshot "t$(printf '%04d' "$t")_press_up"
            snapshot_bottom "t$(printf '%04d' "$t")_press_up"
            break
        fi
    done
    if (( t % 5 == 0 )); then
        snapshot "t$(printf '%04d' "$t")"
        snapshot_bottom "t$(printf '%04d' "$t")"
    fi
done

snapshot t_final
snapshot_bottom t_final
if (( alive )); then
    echo "t=$DUR $(date +%s.%N) SIGTERM (bounded run)" >> "$OUT/timeline.log"
    kill -TERM "$GP" 2>/dev/null
    # the guest does not always honour SIGTERM; escalate so `wait` cannot hang
    for _ in 1 2 3 4 5; do
        kill -0 "$GP" 2>/dev/null || break
        sleep 1
    done
    if kill -0 "$GP" 2>/dev/null; then
        echo "t=$DUR SIGTERM ignored - SIGKILL" >> "$OUT/timeline.log"
        kill -KILL "$GP" 2>/dev/null
    fi
fi
wait "$GP" 2>/dev/null
RC=$?
echo "exit=$RC t_end=$(date +%s.%N)" >> "$OUT/timeline.log"

{
    echo "== label=$LABEL rc=$RC duration=${DUR}s skip_movie=${SKIP_MOVIE:-0} =="
    echo "escapes/unsupported: $(grep -cE 'translated PC escaped|unsupported guest instruction|segmentation fault' "$OUT/run.log")"
    grep -E 'translated PC escaped|unsupported guest instruction|segmentation fault' "$OUT/run.log" | head -5
    echo "HID edges: $(grep -c 'HID buttons' "$OUT/run.log")"
    echo "Y2R conversions: $(grep -c 'Y2R conversion #' "$OUT/run.log")"
    echo "skip-movie log: $(grep -c 'DQ8_SKIP_MOVIE' "$OUT/run.log")"
    grep 'DQ8_SKIP_MOVIE' "$OUT/run.log" | head -5
    echo "play-wait hits: $(grep -c 'guest trace pc=002FA318' "$OUT/run.log")"
    echo "save/FS lines: $(grep -cE 'FS FormatSaveData|native FS path=|FS ControlArchive' "$OUT/run.log")"
    echo "honeySave: $(find "$UD" -name 'honeySave.bin' -printf '%s bytes %TY-%Tm-%Td %TH:%TM:%TS\n' 2>/dev/null)"
    echo "frames: $(ls -1 "$FRAMES"/*.ppm 2>/dev/null | wc -l)"
} | tee "$OUT/summary.txt"

mkdir -p "$OUT/png"
for f in "$FRAMES"/t_final.ppm "$FRAMES"/t_final.bottom.ppm; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .ppm)
    python3 tools/ppm2png.py "$f" "$OUT/png/$name.png" 3 >/dev/null 2>&1 || true
done
echo "evidence: $OUT"
