#!/usr/bin/env bash
# 60 s DSP verification run: audio trace + thread CPU + RSS sampling.
# usage: tools/dsp_verify60.sh LABEL RUNNER
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
LABEL=${1:?label}
BIN=${2:?runner}
OUT="evidence/audio/$LABEL"
CODE=/home/hermes/DQ8/DQ8.code
ROMFS=/home/hermes/DQ8/target/dq8.romfs
[[ -e "$OUT" ]] && { echo "refusing to reuse $OUT" >&2; exit 2; }
mkdir -p "$OUT/userdata"
printf '0\n' > "$OUT/input.txt"
printf 'start %s\n' "$(date +%s.%N)" > "$OUT/timeline.log"
env DQ8_HEADLESS=1 DQ8_DISPLAY=0 DQ8_RUN_GUEST_THREADS=1 \
    DQ8_PROGRESS_SECONDS=30 DQ8_TRACE_AUDIO=1 \
    DQ8_AUDIO_DUMP="$PWD/$OUT/audio.wav" DQ8_AUDIO_SDL=1 SDL_AUDIODRIVER=dummy \
    DQ8_USER_DATA_DIR="$PWD/$OUT/userdata" DQ8_INPUT_FILE="$PWD/$OUT/input.txt" \
    "$BIN" "$CODE" "$ROMFS" > "$OUT/run.log" 2>&1 &
GP=$!
: > "$OUT/samples.log"
for ((t = 1; t <= 60; ++t)); do
  if ! kill -0 "$GP" 2>/dev/null; then break; fi
  case "$t" in
    8|16|24|32|40|48|56)
      printf '1\n' > "$OUT/input.txt"; sleep 1; printf '0\n' > "$OUT/input.txt" ;;
    *) sleep 1 ;;
  esac
  if (( t % 10 == 0 )); then
    printf 't=%s rss_kb=%s\n' "$t" "$(awk '/VmRSS/{print $2}' /proc/$GP/status 2>/dev/null)" >> "$OUT/samples.log"
    ps -L -p "$GP" -o tid=,pcpu=,comm= >> "$OUT/samples.log" 2>/dev/null || true
  fi
done
kill -9 "$GP" 2>/dev/null || true
kill9_at=$(date +%s.%N)
wait "$GP" 2>/dev/null || true
printf 'kill9 %s\n' "$kill9_at" >> "$OUT/timeline.log"
