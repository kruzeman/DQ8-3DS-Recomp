#!/usr/bin/env bash
# t_993e6fb6: drive the new-game route far enough to reach a REAL 3D scene
# (the "Yangus / Oi! Guv!" opening) and snapshot every second, so the GL and SW
# backends can be compared on an actual model scene (the operator's savestate
# only reaches the load menu).
#
# Cadence (from t_c88c7a99's probe5 archaeology, adapted to start from the
# "No previous save data" savestate):
#   t<RECIPE_START            A every 3 s  (dismiss note / skip movies / title)
#   RECIPE_START..ADV_START   [DOWN,A,A,START,A] every 6 s (name entry + confirm)
#   >= ADV_START              A every 6 s  (advance opening cutscene to the 3D scene)
#
# usage: tools/holes_scene_probe.sh <outdir> <gl:0|1> <secs> [state.sav]
set -u
OUT=${1:?outdir}; GL=${2:?0|1}; DUR=${3:?secs}; STATE=${4:-}
W=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=${BIN:-$W/target/dq8-holes}
RECIPE_START=${RECIPE_START:-120}
ADV_START=${ADV_START:-330}
UD=${UD:-/home/hermes/.hermes/profiles/renderer/cache/scratch/ud_scene}
mkdir -p "$OUT" "$UD"
rm -f "$OUT"/t*.ppm "$OUT"/frames.tsv
IN="$OUT/input.txt"
printf '0\n' > "$IN"
cd "$W"
ARGS=()
if [ -n "$STATE" ]; then ARGS=(--load-state "$STATE"); fi
env -u DISPLAY DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT" \
    DQ8_GL="$GL" DQ8_TRACE_INPUT=1 DQ8_RUN_GUEST_THREADS=1 DQ8_ELIDE_RASTER=0 \
    DQ8_FRAME_STATS=1 DQ8_PROGRESS_SECONDS=20 \
    DQ8_INPUT_FILE="$IN" DQ8_USER_DATA_DIR="$UD" \
    "$BIN" DQ8.code dq8.romfs "${ARGS[@]}" > "$OUT/run.log" 2>&1 &
GP=$!
press() { printf '%s\n' "$1" > "$IN"; ( sleep 1; kill -0 $GP 2>/dev/null && printf '0\n' > "$IN" ) & }
recip_i=0
for ((t=1; t<=DUR; t++)); do
  sleep 1
  if ! kill -0 $GP 2>/dev/null; then echo "t=$t process exited" >> "$OUT/frames.tsv"; break; fi
  S="$OUT/dq8-screen.ppm"
  if [ -f "$S" ]; then
    md=$(md5sum "$S" | cut -d' ' -f1)
    cp -f "$S" "$OUT/t$(printf '%04d' $t).ppm" 2>/dev/null || true
    cp -f "$OUT/dq8-screen-bottom.ppm" "$OUT/t$(printf '%04d' $t).bot.ppm" 2>/dev/null || true
    echo "t=$t md5=$md" >> "$OUT/frames.tsv"
  fi
  if (( t < RECIPE_START )); then
    if (( t % 3 == 0 )); then press 1; fi
  elif (( t < ADV_START )); then
    if (( t % 6 == 0 )); then
      recip_i=$((recip_i + 1))
      case $((recip_i % 5)) in
        1) press 128 ;;
        2|3) press 1 ;;
        4) press 8 ;;
        0) press 1 ;;
      esac
    fi
  else
    if (( t % 6 == 0 )); then press 1; fi
  fi
done
printf '0\n' > "$IN"
kill -9 $GP 2>/dev/null; wait $GP 2>/dev/null
echo "done $OUT gl=$GL dur=$DUR"
