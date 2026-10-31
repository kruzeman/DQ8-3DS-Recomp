#!/usr/bin/env bash
# t_0b95f1a7 black-3D probe v2: adaptive-input variant of run_blackworld_probe.sh.
#
# The v1 fixed press schedule derails when tracing slows the guest (the phase
# alignment shifts and the run sticks in name entry).  v2 keeps the proven
# schedule for the boot/menu phase (t<=250, same SPECS as v1), then drives A
# presses adaptively (movie skip / dialog advance) until the target scene is
# detected — a mostly-black top screen with only small UI content (nz<=12000
# for 5 consecutive checks after t>=260) — then stops all input and captures
# BW_TAIL seconds of the scene before exiting.
#
# usage: BW_PREFIX=bw-gl3 BW_EXTRA="DQ8_TRACE_LAYER=250,400" \
#        bash tools/run_blackworld_probe2.sh
#
# Env knobs (all optional):
#   BW_BIN / BW_GL / BW_UD_SRC / BW_CODE / BW_ROMFS / BW_EXTRA — as v1
#   BW_DUR   hard cap seconds (default 600)
#   BW_SNAP  snapshot period seconds (default 2)
#   BW_TAIL  seconds to capture after scene detection (default 120)
#   BW_PREFIX evidence dir name under target/blackworld-evidence/
set -u
WT=${BW_WT:-$(cd "$(dirname "$0")/.." && pwd)}
BIN=${BW_BIN:-/home/hermes/dq8-pkg10/dq8}
GL=${BW_GL:-1}
UD_SRC=${BW_UD_SRC:-}
DUR=${BW_DUR:-600}
PREFIX=${BW_PREFIX:-bw-probe2}
SNAP=${BW_SNAP:-2}
TAIL=${BW_TAIL:-120}
CODE=${BW_CODE:-/home/hermes/DQ8/DQ8.code}
ROMFS=${BW_ROMFS:-/home/hermes/DQ8/target/dq8.romfs}
SPECS=(30 60 90 195 203 207 211:START 216:START 221 226 231:START 236 241 246 250 256 262 268 274 280 286 292 300 310 320 330 340 360 380)
BUTTON_A=0x1; BUTTON_START=0x8
SCENE_NZ=12000

OUT=$WT/target/blackworld-evidence/$PREFIX
mkdir -p "$OUT/frames"
rm -rf "$OUT/userdata"; mkdir -p "$OUT/userdata"
if [ -n "${BW_UD_REUSE:-}" ]; then
  # Reuse a previous run's userdata as-is (contains a game-created adventure
  # log with the slot-used flag set: skips the name-entry flow entirely).
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

echo "t0=$(date +%s.%N) bin=$BIN gl=$GLARG ud=$UD_SRC dur=$DUR snap=$SNAP tail=$TAIL extra=${BW_EXTRA:-}" >> "$OUT/timeline.log"
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

low_streak=0; scene_t=-1; last_snap=0; phase2_next=0; phase2_is_start=0
for ((t=1; t<=DUR; t++)); do
  sleep 1
  if ! kill -0 "$gp" 2>/dev/null; then
    echo "t=$t process exited early" >> "$OUT/timeline.log"; break
  fi
  SCREEN="$OUT/frames/dq8-screen.ppm"
  NZ=-1
  [ -f "$SCREEN" ] && NZ=$(python3 "$WT/tools/ppm_nz.py" "$SCREEN" 2>/dev/null || echo -1)

  # snapshots
  if (( t - last_snap >= SNAP )); then
    last_snap=$t
    [ -f "$SCREEN" ] && cp -f "$SCREEN" "$OUT/frames/t$(printf '%04d' "$t").ppm" 2>/dev/null
    [ -f "$OUT/frames/dq8-screen-bottom.ppm" ] &&
      cp -f "$OUT/frames/dq8-screen-bottom.ppm" "$OUT/frames/t$(printf '%04d' "$t").bottom.ppm" 2>/dev/null
  fi

  # scene detection (mostly-black top screen, after the menu phase)
  if (( scene_t < 0 )); then
    if (( t >= 260 )) && [ "$NZ" -ge 0 ] && (( NZ <= SCENE_NZ )); then
      low_streak=$((low_streak+1))
    else
      low_streak=0
    fi
    if (( low_streak >= 5 )); then
      scene_t=$t
      echo "t=$t SCENE_DETECTED nz=$NZ" >> "$OUT/timeline.log"
    fi
  else
    if (( t - scene_t >= TAIL )); then
      echo "t=$t TAIL_DONE" >> "$OUT/timeline.log"; break
    fi
  fi

  if (( scene_t >= 0 )); then
    continue  # observe mode: no more input
  fi

  # Phase 1: the proven v1 schedule (boot -> menus -> name entry -> movie).
  for spec in "${SPECS[@]}"; do
    btn=A; base=$spec
    case "$spec" in *:*) btn=${spec##*:}; base=${spec%%:*} ;; esac
    case "$btn" in START|start) value=$BUTTON_START ;; *) value=$BUTTON_A ;; esac
    case "$base" in *-*) d=${base%%-*}; u=${base##*-} ;; *) d=$base; u=$((base+1)) ;; esac
    if (( t == d )); then press "$value" "$t" "$btn" "$NZ"; fi
    if (( t == u )); then release; fi
  done

  # Phase 2 (t>250): adaptive advance until the scene shows up.  Name entry
  # (reached later with the clip-fix binary's heavier 3D-background rendering)
  # needs an A,A,A,START burst to confirm, so send that pattern every cycle;
  # the A presses also skip the movie and advance dialogs.
  if (( t > 250 && t >= phase2_next )); then
    phase2_next=$((t+8))
    press "$BUTTON_A" "$t" A "$NZ"
    ( sleep 1; kill -0 "$gp" 2>/dev/null && printf '%s\n' "$BUTTON_A" > "$OUT/input.txt"; \
      sleep 1; kill -0 "$gp" 2>/dev/null && printf '%s\n' "$BUTTON_A" > "$OUT/input.txt"; \
      sleep 1; kill -0 "$gp" 2>/dev/null && printf '%s\n' "$BUTTON_START" > "$OUT/input.txt"; \
      sleep 1; kill -0 "$gp" 2>/dev/null && release ) &
  fi
done
kill -0 "$gp" 2>/dev/null && kill -KILL "$gp" 2>/dev/null
wait "$gp" 2>/dev/null
echo "exit=$? t_end=$(date +%s.%N) scene_t=$scene_t" >> "$OUT/timeline.log"

echo "=== $PREFIX ==="
echo -n "frames snapped: "; ls "$OUT/frames"/t*.ppm 2>/dev/null | wc -l
echo "--- last timeline ---"; tail -8 "$OUT/timeline.log"
echo "--- FRAMESTAT tail ---"; grep "FRAMESTAT" "$OUT/run.log" | tail -4
echo "--- LAYER draws histogram (tail) ---"
grep -E '^LAYER .* draws=' "$OUT/run.log" | sed -E 's/.*draws=([0-9]+).*/\1/' | sort -n | uniq -c | tail -15
echo "--- FAILED draws ---"; grep -c 'DRAW .*FAILED' "$OUT/run.log"
