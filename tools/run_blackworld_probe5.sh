#!/usr/bin/env bash
# t_c88c7a99 probe v5: capture the REAL 3D "Yangus / Oi! Guv!" scene.
#
# Why not v4: (a) v4 had NO phase-1 presses (save dialog sat until t=120);
# (b) v4's 3 A-advance presses right at scene-detect landed mid-opening and
#     bw-c2's run ended at t=396 BEFORE the dialog state appears (bw-fix6
#     reached the dialog only at t~450 after its late A-advance cadence);
# (c) v4 snap=2 s missed short states and F1 capture elision could freeze
#     dq8-screen.ppm on unchanged frames (bw-c2's "frozen" tail).
#
# Flow (verified in bw-fix6 / bw-c2 frame archaeology):
#   t=30 A   -> dismiss "No previous save data"
#   t=60 A   -> skip intro movie #1 (parchment-map FMV)
#   t=90 A   -> title / new-game confirm
#   t=120..279 wander [DOWN,A,A,START,A] every 8s -> name entry recipe
#               (DOWN focuses the letter grid, A types, A types, START opens
#                "Use this name?", A confirms -> "Creating adventure log")
#   t=280..DUR A every 15s -> advance opening cutscene/dialog (bw-fix6 cadence
#               reached the "Yangus / Oi! Guv!" dialog state this way)
#   capture dq8-screen(+bottom).ppm EVERY SECOND, labeled with wall tick.
#
# DQ8_ELIDE_RASTER=0: disable F1 raster/capture elision so the PPM and the
# DRAW trace keep updating on unchanged frames (removes the bw-c2 confound).
#
# usage: BW_BIN=target/dq8-rfix5 BW_PREFIX=bw-c3 bash tools/run_blackworld_probe5.sh
set -u
WT=${BW_WT:-$(cd "$(dirname "$0")/.." && pwd)}
BIN=${BW_BIN:-target/dq8-rfix5}
GL=${BW_GL:-1}
UD_SRC=${BW_UD_SRC:-}
DUR=${BW_DUR:-660}
PREFIX=${BW_PREFIX:-bw-c3}
CODE=${BW_CODE:-/home/hermes/DQ8/DQ8.code}
ROMFS=${BW_ROMFS:-/home/hermes/DQ8/target/dq8.romfs}
BUTTON_A=0x1; BUTTON_START=0x8

OUT=$WT/target/blackworld-evidence/$PREFIX
mkdir -p "$OUT/frames" "$OUT/cmdbin"
rm -rf "$OUT/userdata"; mkdir -p "$OUT/userdata"
if [ -n "${BW_UD_REUSE:-}" ]; then
  [ -d "$BW_UD_REUSE" ] || { echo "missing userdata $BW_UD_REUSE" >&2; exit 1; }
  cp -a "$BW_UD_REUSE/." "$OUT/userdata/"
elif [ -n "$UD_SRC" ]; then
  [ -f "$UD_SRC" ] || { echo "missing save $UD_SRC" >&2; exit 1; }
  mkdir -p "$OUT/userdata/archive-00000004-00000000"
  cp -f "$UD_SRC" "$OUT/userdata/archive-00000004-00000000/honeySave.bin"
fi
printf '0\n' > "$OUT/input.txt"
: > "$OUT/run.log"; : > "$OUT/timeline.log"

EXTRA_ENV=()
[[ -n "${BW_EXTRA:-}" ]] && read -ra EXTRA_ENV <<< "$BW_EXTRA"
GLARG=0; [ "$GL" = 1 ] && GLARG=1

echo "t0=$(date +%s.%N) bin=$BIN gl=$GLARG dur=$DUR extra=${BW_EXTRA:-} probe=v5" >> "$OUT/timeline.log"
env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
    DQ8_PROGRESS_SECONDS=10 DQ8_CAPTURE_PPM=1 \
    DQ8_CAPTURE_PPM_DIR="$OUT/frames" \
    DQ8_ELIDE_RASTER=0 \
    DQ8_FRAME_STATS=1 DQ8_TRACE_GSP_FB=1 \
    DQ8_GL=$GLARG "${EXTRA_ENV[@]}" "$@" \
    DQ8_USER_DATA_DIR="$OUT/userdata" DQ8_INPUT_FILE="$OUT/input.txt" \
    "$BIN" "$CODE" "$ROMFS" > "$OUT/run.log" 2>&1 &
gp=$!

press() { printf '%s\n' "$1" > "$OUT/input.txt"; echo "t=$2 press=$3 nz=$4" >> "$OUT/timeline.log"; }
release() { printf '0\n' > "$OUT/input.txt"; }

wander_next=120; wander_i=0; adv_next=280
for ((t=1; t<=DUR; t++)); do
  sleep 1
  if ! kill -0 "$gp" 2>/dev/null; then
    echo "t=$t process exited early" >> "$OUT/timeline.log"; break
  fi
  SCREEN="$OUT/frames/dq8-screen.ppm"
  NZ=-1
  [ -f "$SCREEN" ] && NZ=$(python3 "$WT/tools/ppm_nz.py" "$SCREEN" 2>/dev/null || echo -1)
  # capture EVERY tick (top + bottom)
  if [ -f "$SCREEN" ]; then
    cp -f "$SCREEN" "$OUT/frames/t$(printf '%04d' "$t").ppm" 2>/dev/null || true
    cp -f "$OUT/frames/dq8-screen-bottom.ppm" "$OUT/frames/t$(printf '%04d' "$t").bottom.ppm" 2>/dev/null || true
  fi

  case $t in
    30|60|90) press "$BUTTON_A" "$t" A-boot "$NZ"; ( sleep 1; kill -0 "$gp" 2>/dev/null && release ) & ;;
  esac

  if (( t >= 120 && t < 280 && t >= wander_next )); then
    wander_next=$((t+8)); wander_i=$((wander_i+1))
    case $((wander_i % 5)) in
      1) press 128 "$t" DOWN "$NZ" ;;
      2|3) press "$BUTTON_A" "$t" A "$NZ" ;;
      4) press "$BUTTON_START" "$t" START "$NZ" ;;
      0) press "$BUTTON_A" "$t" A-yes "$NZ" ;;
    esac
    ( sleep 1; kill -0 "$gp" 2>/dev/null && release ) &
  elif (( t >= 280 && t >= adv_next )); then
    adv_next=$((t+15))
    press "$BUTTON_A" "$t" A-advance "$NZ"
    ( sleep 1; kill -0 "$gp" 2>/dev/null && release ) &
  fi
done
kill -0 "$gp" 2>/dev/null && kill -KILL "$gp" 2>/dev/null
echo "done t_end=$(date +%s.%N)" >> "$OUT/timeline.log"
echo "=== $PREFIX complete: $OUT ==="
