#!/usr/bin/env bash
# t_0b95f1a7 black-3D probe v3: phase-1 = probe2 v2 SPECS + name-entry bursts
# (cab083d), phase-2 = SINGLE presses only (the bursts over-advance the movie/
# cutscene phase into a "waiting for A" state — bw-fix5 evidence), phase-3 =
# a few single A presses after the world appears to advance the cutscene to
# the "Yangus / Oi! Guv!" dialog, then hands-off capture to DUR.
#
# usage: BW_PREFIX=bw-fix6 BW_BIN=target/dq8-rfix4 bash tools/run_blackworld_probe3.sh
set -u
WT=${BW_WT:-$(cd "$(dirname "$0")/.." && pwd)}
BIN=${BW_BIN:-/home/hermes/dq8-pkg10/dq8}
GL=${BW_GL:-1}
UD_SRC=${BW_UD_SRC:-}
DUR=${BW_DUR:-480}
PREFIX=${BW_PREFIX:-bw-probe3}
SNAP=${BW_SNAP:-2}
CODE=${BW_CODE:-/home/hermes/DQ8/DQ8.code}
ROMFS=${BW_ROMFS:-/home/hermes/DQ8/target/dq8.romfs}
SPECS=(30 60 90 195 203 207 211:START 216:START 221 226 231:START 236 241 246 250 256 262 268)
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

echo "t0=$(date +%s.%N) bin=$BIN gl=$GLARG ud=$UD_SRC dur=$DUR snap=$SNAP extra=${BW_EXTRA:-} probe=v3" >> "$OUT/timeline.log"
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

last_snap=0; p2_next=0; p2a_next=0; p3_next=0
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

  # Phase 1: fixed SPECS schedule (boot/menu + name entry)
  for spec in "${SPECS[@]}"; do
    u=${spec%%:*}; b=${spec#*:}; [ "$b" = "$spec" ] && b=A
    if (( t == u )); then
      if [ "$b" = START ]; then press "$BUTTON_START" "$t" START "$NZ"; else press "$BUTTON_A" "$t" A "$NZ"; fi
      ( sleep 1; kill -0 "$gp" 2>/dev/null && release ) &
    fi
  done

  # Phase 1b (t 250..270): name-entry bursts (A,A,A,START) — needed to confirm
  # the name on the clip-fix binaries' heavier screens.
  if (( t > 250 && t <= 270 && t >= p2_next )); then
    p2_next=$((t+8))
    press "$BUTTON_A" "$t" A "$NZ"
    ( sleep 1; kill -0 "$gp" 2>/dev/null && printf '%s\n' "$BUTTON_A" > "$OUT/input.txt"; \
      sleep 1; kill -0 "$gp" 2>/dev/null && printf '%s\n' "$BUTTON_A" > "$OUT/input.txt"; \
      sleep 1; kill -0 "$gp" 2>/dev/null && printf '%s\n' "$BUTTON_START" > "$OUT/input.txt"; \
      sleep 1; kill -0 "$gp" 2>/dev/null && release ) &
  fi

  # Phase 2 (t 275..320): single A presses every ~10s (movie skip / advance).
  if (( t >= 275 && t <= 320 && t >= p2a_next )); then
    p2a_next=$((t+10))
    press "$BUTTON_A" "$t" A "$NZ"
    ( sleep 1; kill -0 "$gp" 2>/dev/null && release ) &
  fi

  # Phase 3 (t 355..405): a few single A presses once the world appears, to
  # advance the post-movie cutscene to the dialog, then hands-off.
  if (( t >= 355 && t <= 405 && t >= p3_next )); then
    p3_next=$((t+12))
    press "$BUTTON_A" "$t" A "$NZ"
    ( sleep 1; kill -0 "$gp" 2>/dev/null && release ) &
  fi
done
kill -0 "$gp" 2>/dev/null && kill -KILL "$gp" 2>/dev/null
echo "done t_end=$(date +%s.%N)" >> "$OUT/timeline.log"
tail -6 "$OUT/run.log" | grep FRAMESTAT > "$OUT/framestat-tail.txt" 2>/dev/null || true
echo "=== $PREFIX complete: $OUT ==="
