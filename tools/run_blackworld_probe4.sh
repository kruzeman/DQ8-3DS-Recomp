#!/usr/bin/env bash
# t_07e30915 probe v4: scene-detection-driven probe (robust where v3's fixed
# wall-clock schedule derails in name entry).
#
# Why: v3's fixed press schedule depends on exact screen timing — bw-fix6 got
# through name entry and reached the scene, bw-fix7/bw-fix8 derailed into
# "That name cannot be used" and never left name entry (wall-clock drift).
#
# Strategy:
#   phase 1 (t<=120): fixed A presses at 30/60/90 (boot/logo/menu).
#   phase 2 (wander): cycle [DOWN, A, A, START, A] every WANDER_S seconds.
#     VERIFIED name-entry recipe (live-guest experiment, bw-c1): the name
#     screen's focus starts OFF the letter grid and A presses do nothing until
#     a D-pad press moves focus in (DOWN lands on a grid letter); then A types
#     the focused letter, START opens the "Use this name?" [Yes/No] popup, and
#     A answers Yes -> "Creating adventure log".  On the movie, A presses skip
#     it; on cutscenes they advance dialog.  A with an EMPTY name + START is
#     what produced the "That name cannot be used" dead-end (bw-fix7/8) — the
#     DOWN+A+A prefix always types before START can confirm.
#   phase 3 (detect): the world scene is reached when >=DETECT_N DRAW lines
#     with count>=500 appear in the run.log tail.  Name entry emits ZERO
#     draws with count>=100 (bw-fix8: 70085 draws, all count<=66, wmin=wmax=1)
#     so this detector cannot false-fire on menus/name entry.
#   phase 4 (settle): 3 single A presses 10s apart to advance the post-movie
#     cutscene to the "Yangus / Oi! Guv!" dialog (v3 phase-3 behaviour), then
#     hands-off capture for BW_TAIL seconds.
#
# usage: BW_PREFIX=bw-c1 BW_BIN=target/dq8-rfix4 bash tools/run_blackworld_probe4.sh
set -u
WT=${BW_WT:-$(cd "$(dirname "$0")/.." && pwd)}
BIN=${BW_BIN:-target/dq8-rfix4}
GL=${BW_GL:-1}
UD_SRC=${BW_UD_SRC:-}
DUR=${BW_DUR:-900}
PREFIX=${BW_PREFIX:-bw-probe4}
SNAP=${BW_SNAP:-2}
TAIL=${BW_TAIL:-120}
CODE=${BW_CODE:-/home/hermes/DQ8/DQ8.code}
ROMFS=${BW_ROMFS:-/home/hermes/DQ8/target/dq8.romfs}
WANDER_S=${BW_WANDER:-6}
DETECT_N=${BW_DETECT_N:-4}
BUTTON_A=0x1; BUTTON_START=0x8

OUT=$WT/target/blackworld-evidence/$PREFIX
mkdir -p "$OUT/frames"
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

echo "t0=$(date +%s.%N) bin=$BIN gl=$GLARG ud=$UD_SRC dur=$DUR snap=$SNAP tail=$TAIL extra=${BW_EXTRA:-} probe=v4" >> "$OUT/timeline.log"
env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
    DQ8_PROGRESS_SECONDS=10 DQ8_CAPTURE_PPM=1 \
    DQ8_CAPTURE_PPM_DIR="$OUT/frames" \
    DQ8_FRAME_STATS=1 DQ8_TRACE_GSP_FB=1 \
    DQ8_GL=$GLARG "${EXTRA_ENV[@]}" "$@" \
    DQ8_USER_DATA_DIR="$OUT/userdata" DQ8_INPUT_FILE="$OUT/input.txt" \
    "$BIN" "$CODE" "$ROMFS" > "$OUT/run.log" 2>&1 &
gp=$!

press() { printf '%s\n' "$1" > "$OUT/input.txt"; echo "t=$2 press=$3 nz=$4" >> "$OUT/timeline.log"; }
release() { printf '0\n' > "$OUT/input.txt"; }

last_snap=0; wander_next=0; wander_i=0
scene_t=-1; settle_left=0; settle_next=0; tail_end=-1
for ((t=1; t<=DUR; t++)); do
  sleep 1
  if ! kill -0 "$gp" 2>/dev/null; then
    echo "t=$t process exited early" >> "$OUT/timeline.log"; break
  fi
  SCREEN="$OUT/frames/dq8-screen.ppm"
  NZ=-1
  [ -f "$SCREEN" ] && NZ=$(python3 "$WT/tools/ppm_nz.py" "$SCREEN" 2>/dev/null || echo -1)

  if (( t - last_snap >= SNAP )); then
    last_snap=$t
    if [ -f "$SCREEN" ]; then
      cp -f "$SCREEN" "$OUT/frames/t$(printf '%04d' "$t").ppm" 2>/dev/null || true
      cp -f "$OUT/frames/dq8-screen-bottom.ppm" "$OUT/frames/t$(printf '%04d' "$t").bottom.ppm" 2>/dev/null || true
    fi
  fi

  # Scene detection: big vertex batches in the recent trace = world scene.
  if (( scene_t < 0 && t >= 120 )) && (( t % 2 == 0 )); then
    BIG=$(tail -c 200000 "$OUT/run.log" 2>/dev/null | grep -c -E 'DRAW .*count=([5-9][0-9][0-9]|[0-9]{4,}) ' || true)
    if (( BIG >= DETECT_N )); then
      scene_t=$t; settle_left=3; settle_next=$((t+2))
      echo "t=$t SCENE-DETECTED big=$BIG nz=$NZ" >> "$OUT/timeline.log"
    fi
  fi

  if (( scene_t < 0 )); then
    # phase 2: wander until the scene appears
    if (( t >= 120 && t >= wander_next )); then
      wander_next=$((t+WANDER_S)); wander_i=$((wander_i+1))
      case $((wander_i % 5)) in
        1) press 128 "$t" DOWN "$NZ" ;;
        2|3) press "$BUTTON_A" "$t" A "$NZ" ;;
        4) press "$BUTTON_START" "$t" START "$NZ" ;;
        0) press "$BUTTON_A" "$t" A-yes "$NZ" ;;
      esac
      ( sleep 1; kill -0 "$gp" 2>/dev/null && release ) &
    fi
  elif (( settle_left > 0 )); then
    # phase 4a: advance the cutscene toward the dialog, then hands-off
    if (( t >= settle_next )); then
      settle_left=$((settle_left-1)); settle_next=$((t+10))
      press "$BUTTON_A" "$t" A-advance "$NZ"
      ( sleep 1; kill -0 "$gp" 2>/dev/null && release ) &
      if (( settle_left == 0 )); then
        tail_end=$((t+TAIL))
        echo "t=$t HANDS-OFF tail_end=$tail_end" >> "$OUT/timeline.log"
      fi
    fi
  else
    # phase 4b: hands-off capture
    if (( t >= tail_end )); then
      echo "t=$t capture-complete" >> "$OUT/timeline.log"; break
    fi
  fi
done
kill -0 "$gp" 2>/dev/null && kill -KILL "$gp" 2>/dev/null
echo "done t_end=$(date +%s.%N) scene_t=$scene_t" >> "$OUT/timeline.log"
tail -6 "$OUT/run.log" | grep FRAMESTAT > "$OUT/framestat-tail.txt" 2>/dev/null || true
echo "=== $PREFIX complete: $OUT ==="
