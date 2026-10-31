#!/usr/bin/env bash
export DQ8_CAPTURE_PPM=1   # PPM screen snapshots are opt-in since audit 5.4
# Automated, bounded DQ8 startup test: boot -> logo -> intro movie -> skip via
# HID A/Start presses -> title screen. Headless (DQ8_HEADLESS=1, no SDL window,
# so no xvfb is needed), time-boxed, and self-contained.
#
# usage: tools/test_startup_title.sh <label> <duration_s> [press_spec ...]
#
#   label       evidence dir becomes target/title-evidence/<label>
#   duration_s  wall-clock seconds to keep the guest running (hard bound)
#   press_spec  <second>[:<button>]       1000 ms press at that second.
#               <down>-<up>[:<button>]    hold the button from <down> up to
#                                         (but not including) <up> seconds.
#                                         e.g. 340-460:A = one 120 s hold.
#               button is A (default), B or START. Several specs may overlap;
#               the effective button mask is the OR of all active holds.
#
# Environment:
#   TITLE_BIN      binary to run        (default ./target/dq8-startup-split)
#   TITLE_USERDATA prepared save/extra data dir that is COPIED read-only-ish
#                  into the run dir   (default target/userdata-title-1)
#   TITLE_EXTRA    extra env assignments (e.g. 'DQ8_TRACE_Y2R=1')
#
# DQ8_SKIP_MOVIE=1 (exported in the environment) skips the opening movie so the
# run reaches the post-intro main-menu state in seconds; unset plays it normally.
#
# The prepared save dir is never written to: every run works on its own copy,
# so the guest can create/modify whatever it wants inside the run dir.
set -u
cd /home/hermes/DQ8 || exit 1

LABEL=${1:?label}
DUR=${2:?duration}
shift 2
SPECS=("$@")

BIN=${TITLE_BIN:-./target/dq8-startup-split}
UD_SRC=${TITLE_USERDATA:-target/userdata-title-1}
OUT=target/title-evidence/$LABEL
UD=$OUT/userdata
FRAMES=$OUT/frames
INPUT=$OUT/input.txt
PPM_FULL=288015   # "P6\n400 240\n255\n" + 400*240*3
PPM_BOT_FULL=23055  # "P6\n320 240\n255\n" + 320*240*3

BUTTON_A=0x1
BUTTON_B=0x2
BUTTON_START=0x8

if [ ! -x "$BIN" ]; then echo "missing binary $BIN" >&2; exit 1; fi
if [ ! -d "$UD_SRC" ]; then echo "missing prepared save dir $UD_SRC" >&2; exit 1; fi
mkdir -p "$FRAMES"
if [ ! -d "$UD" ]; then cp -a "$UD_SRC" "$UD"; fi
printf '0\n' > "$INPUT"
rm -f /tmp/dq8-screen.ppm /tmp/dq8-screen-bottom.ppm
: > "$OUT/run.log"
: > "$OUT/timeline.log"

log() { echo "$*" >> "$OUT/timeline.log"; }

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

# The runtime snapshots both presented screens each frame (top:
# /tmp/dq8-screen.ppm, bottom: /tmp/dq8-screen-bottom.ppm); the bottom
# (320x240) screen is where 3DS menus live, so it is captured next to the top.
snapshot_bottom() {
    if [ -f /tmp/dq8-screen-bottom.ppm ]; then
        local size
        size=$(stat -c %s /tmp/dq8-screen-bottom.ppm 2>/dev/null || echo 0)
        if [ "$size" -ge "$PPM_BOT_FULL" ]; then
            cp -f /tmp/dq8-screen-bottom.ppm "$FRAMES/$1.bottom.ppm" 2>/dev/null || true
        fi
    fi
}

press_value() {
    case "$1" in
        A|a) echo "$BUTTON_A" ;;
        B|b) echo "$BUTTON_B" ;;
        START|start|Start) echo "$BUTTON_START" ;;
        *) echo "$1" ;;
    esac
}

# Normalize press specs into parallel arrays: hold window [down, up) + value.
# `<t>:BTN` becomes a 1000 ms press (t .. t+1); `d-u:BTN` is a long hold.
HOLD_D=(); HOLD_U=(); HOLD_V=()
for spec in ${SPECS[@]+"${SPECS[@]}"}; do
    btn=A
    base=$spec
    case "$spec" in *:*) btn=${spec##*:}; base=${spec%%:*} ;; esac
    value=$(press_value "$btn")
    case "$base" in
        *-*) d=${base%%-*}; u=${base##*-} ;;
        *)   d=$base; u=$((base + 1)) ;;
    esac
    if ! [[ "$d" =~ ^[0-9]+$ && "$u" =~ ^[0-9]+$ ]] || (( u <= d )); then
        echo "bad press spec: $spec" >&2; exit 1
    fi
    HOLD_D+=("$d"); HOLD_U+=("$u"); HOLD_V+=("$value")
    log "spec $spec -> hold [$d,$u) value=$value"
done

# shellcheck disable=SC2086
env DQ8_HEADLESS=1 DQ8_DISPLAY=0 DQ8_RUN_GUEST_THREADS=1 \
    DQ8_TRACE_IPC=1 DQ8_TRACE_INPUT=1 DQ8_PROGRESS_SECONDS=1 \
    DQ8_USER_DATA_DIR="$UD" DQ8_INPUT_FILE="$INPUT" ${TITLE_EXTRA:-} \
    "$BIN" DQ8.code target/dq8.romfs > "$OUT/run.log" 2>&1 &
GP=$!
log "t0=$(date +%s.%N) pid=$GP label=$LABEL bin=$BIN userdata_src=$UD_SRC userdata=$UD duration=$DUR specs=${SPECS[*]:-none} extra=${TITLE_EXTRA:-none}"

alive=1
prev_mask=0
for ((t = 1; t <= DUR; t++)); do
    sleep 1
    if ! kill -0 "$GP" 2>/dev/null; then
        log "t=$t process exited early"
        alive=0
        break
    fi
    # effective mask = OR of every hold window containing this second
    mask=0
    for i in "${!HOLD_D[@]}"; do
        if (( t >= HOLD_D[i] && t < HOLD_U[i] )); then mask=$((mask | HOLD_V[i])); fi
    done
    if (( mask != prev_mask )); then
        down_m=$((mask & ~prev_mask)); up_m=$((prev_mask & ~mask))
        log "t=$t $(date +%s.%N) DOWN=0x$(printf '%X' "$down_m") UP=0x$(printf '%X' "$up_m") mask=0x$(printf '%X' "$mask")"
        printf '%s\n' "$mask" > "$INPUT"
        if (( down_m )); then
            snapshot "t$(printf '%04d' "$t")_press_down"
            snapshot_bottom "t$(printf '%04d' "$t")_press_down"
        fi
        if (( up_m )); then
            snapshot "t$(printf '%04d' "$t")_press_up"
            snapshot_bottom "t$(printf '%04d' "$t")_press_up"
        fi
        prev_mask=$mask
    fi
    if (( t % 5 == 0 )); then snapshot "t$(printf '%04d' "$t")"; snapshot_bottom "t$(printf '%04d' "$t")"; fi
done

if (( alive )); then
    log "t=$DUR $(date +%s.%N) SIGTERM (clean shutdown, bounded run)"
    kill -TERM "$GP" 2>/dev/null
fi
wait "$GP" 2>/dev/null
RC=$?
log "exit=$RC t_end=$(date +%s.%N)"
snapshot t_final

# ---- report -----------------------------------------------------------------
{
    echo "== label=$LABEL rc=$RC duration=${DUR}s =="
    echo "escapes/unsupported: $(grep -cE 'translated PC escaped|unsupported guest instruction|segmentation fault' "$OUT/run.log")"
    grep -E 'translated PC escaped|unsupported guest instruction|segmentation fault' "$OUT/run.log" | head -5
    echo "HID edges: $(grep -c 'HID buttons' "$OUT/run.log")"
    grep 'HID buttons' "$OUT/run.log" | head -20
    echo "Y2R conversions: $(grep -c 'Y2R conversion #' "$OUT/run.log")"
    echo "last conversion: $(grep 'Y2R conversion #' "$OUT/run.log" | tail -1)"
    echo "frames: $(ls -1 "$FRAMES" | wc -l)"
    if [ -f "$FRAMES/t_final.ppm" ]; then
        python3 tools/ppm_stats.py "$FRAMES/t_final.ppm"
    fi
} | tee "$OUT/summary.txt"

mkdir -p "$OUT/png"
for f in "$FRAMES"/t_final.ppm "$FRAMES"/t_final.bottom.ppm "$FRAMES"/t*_press_down.ppm "$FRAMES"/t*_press_down.bottom.ppm "$FRAMES"/t*_press_up.ppm "$FRAMES"/t*_press_up.bottom.ppm; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .ppm)
    python3 tools/ppm2png.py "$f" "$OUT/png/$name.png" 3 >/dev/null 2>&1 || true
done
echo "evidence: $OUT (log, timeline.log, frames/, png/, summary.txt)"
