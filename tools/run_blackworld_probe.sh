#!/usr/bin/env bash
# t_0b95f1a7 black-3D probe: drive the game to an in-world scene (Yangus
# dialogue / post-save-load game world) headless and capture top-screen PPMs +
# renderer counters so the black-3D bug can be reproduced and A/B'd.
#
# usage: BW_PREFIX=bw-gl BW_GL=1 BW_BIN=/home/hermes/dq8-pkg10/dq8 \
#        BW_UD_SRC=/home/hermes/DQ8/target/adventure-log-evidence/honeySave-run3.bin \
#        BW_DUR=420 bash tools/run_blackworld_probe.sh
#
# Env knobs:
#   BW_BIN     binary to run (default /home/hermes/dq8-pkg10/dq8)
#   BW_GL      1 = DQ8_GL=1 (GL path), 0 = software (default 1)
#   BW_UD_SRC  honeySave.bin source (file). If empty: fresh userdata.
#   BW_DUR     run length seconds (default 420)
#   BW_PREFIX  evidence dir name under target/blackworld-evidence/
#   BW_EXTRA   extra env assignments (e.g. "DQ8_ELIDE_RASTER=0 DQ8_GL_BATCH=0")
#   BW_SNAP    snapshot period seconds (default 5)
set -u
WT=${BW_WT:-$(cd "$(dirname "$0")/.." && pwd)}
BIN=${BW_BIN:-/home/hermes/dq8-pkg10/dq8}
GL=${BW_GL:-1}
UD_SRC=${BW_UD_SRC:-}
DUR=${BW_DUR:-420}
PREFIX=${BW_PREFIX:-bw-probe}
SNAP=${BW_SNAP:-5}
CODE=${BW_CODE:-/home/hermes/DQ8/DQ8.code}
ROMFS=${BW_ROMFS:-/home/hermes/DQ8/target/dq8.romfs}
SPECS=(30 60 90 195 203 207 211:START 216:START 221 226 231:START 236 241 246 250 256 262 268 274 280 286 292 300 310 320 330 340 360 380)
BUTTON_A=0x1; BUTTON_START=0x8

OUT=$WT/target/blackworld-evidence/$PREFIX
mkdir -p "$OUT/frames"
rm -rf "$OUT/userdata"; mkdir -p "$OUT/userdata"
if [ -n "$UD_SRC" ]; then
  [ -f "$UD_SRC" ] || { echo "missing save $UD_SRC" >&2; exit 1; }
  mkdir -p "$OUT/userdata/archive-00000004-00000000"
  cp -f "$UD_SRC" "$OUT/userdata/archive-00000004-00000000/honeySave.bin"
fi
printf '0\n' > "$OUT/input.txt"
: > "$OUT/run.log"; : > "$OUT/timeline.log"

EXTRA_ENV=()
[[ -n "${BW_EXTRA:-}" ]] && read -ra EXTRA_ENV <<< "$BW_EXTRA"
GLARG=0; [ "$GL" = 1 ] && GLARG=1

echo "t0=$(date +%s.%N) bin=$BIN gl=$GLARG ud=$UD_SRC dur=$DUR extra=${BW_EXTRA:-}" >> "$OUT/timeline.log"
env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
    DQ8_PROGRESS_SECONDS=10 DQ8_CAPTURE_PPM=1 \
    DQ8_CAPTURE_PPM_DIR="$OUT/frames" \
    DQ8_FRAME_STATS=1 DQ8_TRACE_GSP_FB=1 \
    DQ8_GL=$GLARG "${EXTRA_ENV[@]}" "$@" \
    DQ8_USER_DATA_DIR="$OUT/userdata" DQ8_INPUT_FILE="$OUT/input.txt" \
    "$BIN" "$CODE" "$ROMFS" > "$OUT/run.log" 2>&1 &
gp=$!
for ((t=1; t<=DUR; t++)); do
  sleep 1
  if ! kill -0 "$gp" 2>/dev/null; then
    echo "t=$t process exited early" >> "$OUT/timeline.log"; break
  fi
  if (( t % SNAP == 0 )); then
    [ -f "$OUT/frames/dq8-screen.ppm" ] &&
      cp -f "$OUT/frames/dq8-screen.ppm" "$OUT/frames/t$(printf '%04d' "$t").ppm" 2>/dev/null
    [ -f "$OUT/frames/dq8-screen-bottom.ppm" ] &&
      cp -f "$OUT/frames/dq8-screen-bottom.ppm" "$OUT/frames/t$(printf '%04d' "$t").bottom.ppm" 2>/dev/null
  fi
  for spec in "${SPECS[@]}"; do
    btn=A; base=$spec
    case "$spec" in *:*) btn=${spec##*:}; base=${spec%%:*} ;; esac
    case "$btn" in START|start) value=$BUTTON_START ;; *) value=$BUTTON_A ;; esac
    case "$base" in *-*) d=${base%%-*}; u=${base##*-} ;; *) d=$base; u=$((base+1)) ;; esac
    if (( t == d )); then printf '%s\n' "$value" > "$OUT/input.txt"; fi
    if (( t == u )); then printf '0\n' > "$OUT/input.txt"; fi
  done
done
kill -0 "$gp" 2>/dev/null && kill -KILL "$gp" 2>/dev/null
wait "$gp" 2>/dev/null
echo "exit=$? t_end=$(date +%s.%N)" >> "$OUT/timeline.log"

echo "=== $PREFIX ==="
echo -n "frames snapped: "; ls "$OUT/frames"/t*.ppm 2>/dev/null | wc -l
echo "--- gl draws log (first/last) ---"
grep -E "GL draw|pica_gl_draws" "$OUT/run.log" | head -5
grep -E "GL draw|pica_gl_draws" "$OUT/run.log" | tail -5
echo "--- FRAMESTAT ---"
grep "FRAMESTAT" "$OUT/run.log" | tail -6
echo "--- escapes ---"; grep -c "escaped to\|guest memory fault" "$OUT/run.log"
tail -2 "$OUT/timeline.log"
