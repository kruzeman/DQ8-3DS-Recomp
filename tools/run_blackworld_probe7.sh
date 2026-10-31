#!/usr/bin/env bash
# probe v7 = probe v5 with a HOLD WINDOW at the "Oi! Guv!" line.
# bw-c3 archaeology: the opening cutscene starts ~t405 (screen black -> fade
# in), line 1 ("Oi! Guv!") is on screen t416-434, and probe5's A-advance at
# t=430 advanced it BEFORE the scene finished fading in (nz 13k -> 53k at
# t441, when line 2 was already typing).  probe7 therefore presses the same
# boot/wander/advance schedule up to t=415, then HOLDS A from t416..t469 so
# line 1 stays up while the fade completes, captures every second, and
# resumes advancing at t=470.
set -u
WT=${BW_WT:-$(cd "$(dirname "$0")/.." && pwd)}
BIN=${BW_BIN:-target/dq8-rfix5}
GL=${BW_GL:-1}
DUR=${BW_DUR:-660}
PREFIX=${BW_PREFIX:-bw-oi1}
CODE=${BW_CODE:-/home/hermes/DQ8/DQ8.code}
ROMFS=${BW_ROMFS:-/home/hermes/DQ8/target/dq8.romfs}
BUTTON_A=0x1; BUTTON_START=0x8
HOLD_FROM=${HOLD_FROM:-416}
HOLD_TO=${HOLD_TO:-469}

OUT=$WT/target/blackworld-evidence/$PREFIX
mkdir -p "$OUT/frames"
rm -rf "$OUT/userdata"; mkdir -p "$OUT/userdata"
printf '0\n' > "$OUT/input.txt"
: > "$OUT/run.log"; : > "$OUT/timeline.log"

EXTRA_ENV=()
[[ -n "${BW_EXTRA:-}" ]] && read -ra EXTRA_ENV <<< "$BW_EXTRA"
GLARG=0; [ "$GL" = 1 ] && GLARG=1

echo "t0=$(date +%s.%N) bin=$BIN gl=$GLARG dur=$DUR extra=${BW_EXTRA:-} probe=v7 hold=$HOLD_FROM..$HOLD_TO" >> "$OUT/timeline.log"
env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
    DQ8_PROGRESS_SECONDS=10 DQ8_CAPTURE_PPM=1 \
    DQ8_CAPTURE_PPM_DIR="$OUT/frames" \
    DQ8_ELIDE_RASTER=0 \
    DQ8_FRAME_STATS=1 \
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
    if (( t >= HOLD_FROM && t <= HOLD_TO )); then
      echo "t=$t HOLD (line-1 window, no press) nz=$NZ" >> "$OUT/timeline.log"
      adv_next=$((HOLD_TO+1))
    else
      adv_next=$((t+15))
      press "$BUTTON_A" "$t" A-advance "$NZ"
      ( sleep 1; kill -0 "$gp" 2>/dev/null && release ) &
    fi
  fi
done
kill -0 "$gp" 2>/dev/null && kill -KILL "$gp" 2>/dev/null
echo "done t_end=$(date +%s.%N)" >> "$OUT/timeline.log"
echo "=== $PREFIX complete: $OUT ==="
