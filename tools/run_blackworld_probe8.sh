#!/usr/bin/env bash
# probe v8 = input-class test at the stuck "Oi! Guv!" line (line 1).
# bw-oi1 evidence: during line 1 the 3D scene is frozen (nz=13099 strip) and
# only an A-advance wakes it (text also advances -> line 2, so A destroys the
# acceptance frame). This probe, once line 1 is on screen (white>=500),
# presses NON-ADVANCING keys (DOWN, B, UP) one every 15 s and watches for a
# wake (nz>=30000 while line 1 text still shown). On wake it STOPS all input
# so the line-1 + lit-scene frame is preserved. If no wake after 6 tries, it
# presses A once (documents the A-only behaviour).
set -u
WT=${BW_WT:-$(cd "$(dirname "$0")/.." && pwd)}
BIN=${BW_BIN:-target/dq8-rfix5}
GL=${BW_GL:-1}
DUR=${BW_DUR:-840}
PREFIX=${BW_PREFIX:-bw-oi2}
CODE=${BW_CODE:-/home/hermes/DQ8/DQ8.code}
ROMFS=${BW_ROMFS:-/home/hermes/DQ8/target/dq8.romfs}
BUTTON_A=0x1; BUTTON_B=0x2; BUTTON_START=0x8; DOWN=128; UP=64

OUT=$WT/target/blackworld-evidence/$PREFIX
mkdir -p "$OUT/frames"
rm -rf "$OUT/userdata"; mkdir -p "$OUT/userdata"
printf '0\n' > "$OUT/input.txt"
: > "$OUT/run.log"; : > "$OUT/timeline.log"

EXTRA_ENV=()
[[ -n "${BW_EXTRA:-}" ]] && read -ra EXTRA_ENV <<< "$BW_EXTRA"
GLARG=0; [ "$GL" = 1 ] && GLARG=1

echo "t0=$(date +%s.%N) bin=$BIN gl=$GLARG dur=$DUR extra=${BW_EXTRA:-} probe=v8" >> "$OUT/timeline.log"
env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
    DQ8_PROGRESS_SECONDS=10 DQ8_CAPTURE_PPM=1 \
    DQ8_CAPTURE_PPM_DIR="$OUT/frames" \
    DQ8_ELIDE_RASTER=0 \
    DQ8_FRAME_STATS=1 \
    DQ8_GL=$GLARG "${EXTRA_ENV[@]}" "$@" \
    DQ8_USER_DATA_DIR="$OUT/userdata" DQ8_INPUT_FILE="$OUT/input.txt" \
    "$BIN" "$CODE" "$ROMFS" > "$OUT/run.log" 2>&1 &
gp=$!

press() { printf '%s\n' "$1" > "$OUT/input.txt"; echo "t=$2 press=$3 val=$1 nz=$4" >> "$OUT/timeline.log"; }
release() { printf '0\n' > "$OUT/input.txt"; }

wander_next=120; wander_i=0; adv_next=280
line1_seen=0; woke=0; frozen=0; tries=0; next_try=0
TRYKEYS=("$DOWN" "$DOWN" "$BUTTON_B" "$BUTTON_B" "$UP" "$UP")
for ((t=1; t<=DUR; t++)); do
  sleep 1
  if ! kill -0 "$gp" 2>/dev/null; then
    echo "t=$t process exited early" >> "$OUT/timeline.log"; break
  fi
  SCREEN="$OUT/frames/dq8-screen.ppm"
  NZ=-1; WHITE=-1
  if [ -f "$SCREEN" ]; then
    NZ=$(python3 "$WT/tools/ppm_nz.py" "$SCREEN" 2>/dev/null || echo -1)
    WHITE=$(python3 "$WT/tools/wppm.py" "$SCREEN" 2>/dev/null || echo -1)
    cp -f "$SCREEN" "$OUT/frames/t$(printf '%04d' "$t").ppm" 2>/dev/null || true
    cp -f "$OUT/frames/dq8-screen-bottom.ppm" "$OUT/frames/t$(printf '%04d' "$t").bottom.ppm" 2>/dev/null || true
  fi

  # line-1 detection: white text on screen after the pre-movie phase
  if (( t >= 400 && line1_seen == 0 && WHITE >= 500 )); then
    line1_seen=1; frozen=$t; next_try=$((t+8))
    echo "t=$t LINE1-SEEN white=$WHITE nz=$NZ (window starts)" >> "$OUT/timeline.log"
  fi

  # wake detection: line 1 still shown but scene lit
  if (( line1_seen == 1 && woke == 0 && WHITE >= 500 && NZ >= 30000 )); then
    woke=$t
    echo "t=$t WAKE white=$WHITE nz=$NZ tries=$tries -- FREEZING INPUT" >> "$OUT/timeline.log"
  fi

  if (( line1_seen == 1 && woke == 0 )); then
    if (( t >= next_try )); then
      if (( tries < ${#TRYKEYS[@]} )); then
        k=${TRYKEYS[$tries]}
        press "$k" "$t" "probe-key-$tries" "$NZ"
        ( sleep 2; kill -0 "$gp" 2>/dev/null && release ) &
        tries=$((tries+1)); next_try=$((t+15))
      elif (( tries == ${#TRYKEYS[@]} )); then
        press "$BUTTON_A" "$t" "A-fallback" "$NZ"
        ( sleep 2; kill -0 "$gp" 2>/dev/null && release ) &
        tries=$((tries+1)); next_try=$((t+15))
      fi
    fi
  elif (( line1_seen == 0 )); then
    # pre-line1 choreography identical to probe5/7
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
  fi
  # woke==1: no further input at all (freeze the acceptance frame)
done
kill -0 "$gp" 2>/dev/null && kill -KILL "$gp" 2>/dev/null
echo "done t_end=$(date +%s.%N) line1_seen=$line1_seen woke=$woke tries=$tries" >> "$OUT/timeline.log"
echo "=== $PREFIX complete: $OUT ==="
