#!/usr/bin/env bash
# usage: run_title.sh <tag> <gl:0|1> <duration> [press specs...]
# Replica of tools/test_startup_title.sh (commit lineage f826f3f/depth-bpp-fix3)
# for THIS worktree: same env, same press-spec grammar, same /tmp snapshot
# capture, fresh empty userdata, intro movie NOT skipped - the recipe that
# reached the multi-window naming screen (layer-bug-refs/depth-top4-*).
set -u
cd "$(dirname "$0")/.."
TAG=$1; GL=$2; DUR=${3:-270}
shift 3
SPECS=("$@")
DIR=/home/hermes/.hermes/kanban/workspaces/t_667c2f8b/caps9/$TAG
FRAMES=$DIR/frames
UD=$DIR/userdata
INPUT=$DIR/input.txt
UD_SRC=${TITLE_USERDATA:-/home/hermes/DQ8/target/layer-bug-refs/userdata-empty}
PPM_FULL=288015     # P6 400x240 + payload
PPM_BOT_FULL=23055  # P6 320x240 + payload
BUTTON_A=0x1; BUTTON_B=0x2; BUTTON_START=0x8
mkdir -p "$FRAMES"
if [ ! -d "$UD" ]; then cp -a "$UD_SRC" "$UD"; fi
printf '0\n' > "$INPUT"
: > "$DIR/run.log"
: > "$DIR/timeline.log"
log() { echo "$*" >> "$DIR/timeline.log"; }

snapshot() {
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
snapshot_bottom() {
    local size
    if [ -f /tmp/dq8-screen-bottom.ppm ]; then
        size=$(stat -c %s /tmp/dq8-screen-bottom.ppm 2>/dev/null || echo 0)
        if [ "$size" -ge "$PPM_BOT_FULL" ]; then
            cp -f /tmp/dq8-screen-bottom.ppm "$FRAMES/$1.bottom.ppm" 2>/dev/null || true
        fi
    fi
}
press_value() {
    case "$1" in
        A|a) echo "$BUTTON_A" ;;  B|b) echo "$BUTTON_B" ;;
        START|start|Start) echo "$BUTTON_START" ;;  *) echo "$1" ;;
    esac
}
HOLD_D=(); HOLD_U=(); HOLD_V=()
for spec in ${SPECS[@]+"${SPECS[@]}"}; do
    btn=A; base=$spec
    case "$spec" in *:*) btn=${spec##*:}; base=${spec%%:*} ;; esac
    value=$(press_value "$btn")
    case "$base" in *-*) d=${base%%-*}; u=${base##*-} ;; *) d=$base; u=$((base + 1)) ;; esac
    if ! [[ "$d" =~ ^[0-9]+$ && "$u" =~ ^[0-9]+$ ]] || (( u <= d )); then
        echo "bad press spec: $spec" >&2; exit 1
    fi
    HOLD_D+=("$d"); HOLD_U+=("$u"); HOLD_V+=("$value")
    log "spec $spec -> hold [$d,$u) value=$value"
done

BIN=${BIN:-./target/dq8-gl}
if [ "$GL" = "1" ]; then export DQ8_GL=1; else unset DQ8_GL; fi
env DQ8_HEADLESS=1 DQ8_DISPLAY=0 DQ8_RUN_GUEST_THREADS=1 \
    DQ8_TRACE_IPC=1 DQ8_TRACE_INPUT=1 DQ8_PROGRESS_SECONDS=1 \
    DQ8_CAPTURE_PPM=1 \
    DQ8_DUMP_TRANSFER_PREFIX="$DIR/depth-top" DQ8_DUMP_TRANSFER_LATEST=1 \
    DQ8_USER_DATA_DIR="$UD" DQ8_INPUT_FILE="$INPUT" \
    "$BIN" DQ8.code dq8.romfs > "$DIR/run.log" 2>&1 &
GP=$!
log "t0=$(date +%s.%N) pid=$GP tag=$TAG gl=$GL duration=$DUR specs=${SPECS[*]:-none}"

alive=1; prev_mask=0
for ((t = 1; t <= DUR; t++)); do
    sleep 1
    if ! kill -0 "$GP" 2>/dev/null; then log "t=$t process exited early"; alive=0; break; fi
    mask=0
    for i in "${!HOLD_D[@]}"; do
        if (( t >= HOLD_D[i] && t < HOLD_U[i] )); then mask=$((mask | HOLD_V[i])); fi
    done
    if (( mask != prev_mask )); then
        down_m=$((mask & ~prev_mask)); up_m=$((prev_mask & ~mask))
        log "t=$t $(date +%s.%N) DOWN=0x$(printf '%X' "$down_m") UP=0x$(printf '%X' "$up_m") mask=0x$(printf '%X' "$mask")"
        printf '%s\n' "$mask" > "$INPUT"
        if (( down_m )); then snapshot "t$(printf '%04d' "$t")_press_down"; snapshot_bottom "t$(printf '%04d' "$t")_press_down"; fi
        if (( up_m )); then snapshot "t$(printf '%04d' "$t")_press_up"; snapshot_bottom "t$(printf '%04d' "$t")_press_up"; fi
        prev_mask=$mask
    fi
    if (( t % 5 == 0 )); then snapshot "t$(printf '%04d' "$t")"; snapshot_bottom "t$(printf '%04d' "$t")"; fi
done
if (( alive )); then log "t=$DUR SIGTERM (bounded run)"; kill -TERM "$GP" 2>/dev/null; fi
# Bounded shutdown: some guest states ignore SIGTERM; never block here.
for _i in 1 2 3 4 5; do kill -0 "$GP" 2>/dev/null || break; sleep 1; done
kill -9 "$GP" 2>/dev/null || true
wait "$GP" 2>/dev/null
log "exit=$? t_end=$(date +%s.%N)"
snapshot t_final; snapshot_bottom t_final
{
    echo "== tag=$TAG gl=$GL duration=${DUR}s =="
    echo "escapes: $(grep -cE 'translated PC escaped|unsupported guest instruction|segmentation fault' "$DIR/run.log")"
    grep -E 'translated PC escaped|unsupported guest instruction|segmentation fault' "$DIR/run.log" | head -5
    echo "HID edges: $(grep -c 'HID buttons' "$DIR/run.log")"
    echo "Y2R conversions: $(grep -c 'Y2R conversion #' "$DIR/run.log")"
    echo "frames: $(ls -1 "$FRAMES" | wc -l)"
} >> "$DIR/report.txt" 2>&1
cat "$DIR/report.txt"
