#!/usr/bin/env bash
# End-to-end H2 A/B: exact H1+BGM baseline vs audio workers, identical seeded
# movie input/step budget, WAV + spectrogram, thread CPU, and fixed-step frames.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BASE=${DQ8_H2_BASELINE_BIN:-/home/hermes/.hermes/profiles/coder/cache/scratch/dq8-h2-baseline-572206e/target/dq8-audio-baseline}
CAND=${DQ8_H2_CANDIDATE_BIN:-target/dq8-audio-workers}
STEPS=${DQ8_H2_AUDIO_STEPS:-300000000}
OUT=target/audio-workers-evidence
BASE_LABEL=${DQ8_H2_BASELINE_LABEL:-baseline-movie}
CANDIDATE_LABEL=${DQ8_H2_CANDIDATE_LABEL:-candidate-movie-r2}
RUN_LABEL=${DQ8_H2_RUN_LABEL:-h2-r2}
CODE_CHECK="$OUT/generated-code-check-$RUN_LABEL.txt"
CPU_REPORT="$OUT/main-thread-cpu-$RUN_LABEL.txt"

for ((i = 0; i < 600; ++i)); do
  [[ -x "$BASE" ]] && break
  sleep 1
done
[[ -x "$BASE" && -x "$CAND" ]] || { echo "baseline/candidate runner missing" >&2; exit 2; }
mkdir -p target "$OUT"
if [[ ! -e target/dq8.romfs ]]; then ln -s /home/hermes/DQ8/target/dq8.romfs target/dq8.romfs; fi
[[ ! -e "$CODE_CHECK" && ! -e "$CPU_REPORT" && ! -e "$OUT/timeline-$RUN_LABEL.log" ]] || {
  echo "verification output already exists; choose a fresh DQ8_H2_RUN_LABEL" >&2
  exit 7
}

# Compare the generated guest dispatch parts directly; runtime header changes
# are intentional, translated guest instruction bodies must remain identical.
part_count=0
part_mismatch=0
for part in target/dq8-split-audio/dq8_part_*.c; do
  name=${part##*/}
  part_count=$((part_count + 1))
  if ! cmp -s "$part" "/home/hermes/.hermes/profiles/coder/cache/scratch/dq8-h2-baseline-572206e/target/dq8-split-baseline/$name"; then
    printf 'guest part differs: %s\n' "$name" >&2
    part_mismatch=1
  fi
done
printf 'guest_translation_parts=%s byte_identical=%s\n' "$part_count" "$((1 - part_mismatch))" > "$CODE_CHECK"
(( part_mismatch == 0 )) || exit 3

capture_complete() {
  local label=$1
  grep -Fq "BENCH steps=${STEPS}" "$OUT/$label/run.log"
}
ensure_capture() {
  local label=$1 bin=$2
  if [[ -e "$OUT/$label" ]]; then
    if ! capture_complete "$label"; then
      echo "existing capture is incomplete; preserve it and choose a fresh label: $OUT/$label" >&2
      exit 4
    fi
  else
    bash tools/run_audio_worker_capture.sh "$label" "$bin" "$STEPS"
  fi
}

# Each capture uses fresh isolated userdata, the same A-hold schedule, and a
# fixed main-guest-step endpoint. Completed captures are reused read-only.
ensure_capture "$BASE_LABEL" "$BASE"
ensure_capture "$CANDIDATE_LABEL" "$CAND"

[[ ! -e "$OUT/$CANDIDATE_LABEL/audio-analysis.txt" && ! -e "$OUT/$CANDIDATE_LABEL/audio-spec.png" ]] || {
  echo "candidate analysis artifacts already exist; preserve them and choose a fresh label" >&2
  exit 5
}
AUDIO_ANALYSIS="$OUT/$CANDIDATE_LABEL/audio-repaired.wav"
if [[ ! -f "$AUDIO_ANALYSIS" ]]; then
  cp "$OUT/$CANDIDATE_LABEL/audio.wav" "$AUDIO_ANALYSIS"
  python3 tools/fix_wav_header.py "$AUDIO_ANALYSIS"
fi
python3 tools/analyze_audio_wav.py "$AUDIO_ANALYSIS" > "$OUT/$CANDIDATE_LABEL/audio-analysis.txt"
ffmpeg -hide_banner -loglevel error -y -i "$AUDIO_ANALYSIS" \
  -lavfi "showspectrumpic=s=2048x1024:legend=1:scale=log:color=rainbow" \
  -frames:v 1 "$OUT/$CANDIDATE_LABEL/audio-spec.png"
python3 tools/audio_cpu_share.py "$OUT/$BASE_LABEL" "$OUT/$CANDIDATE_LABEL" > "$CPU_REPORT"

for steps in 2000000 5000000 10000000 20000000; do
  frames="$OUT/frames-$RUN_LABEL-$steps"
  [[ ! -e "$frames" ]] || { echo "frame evidence already exists: $frames" >&2; exit 6; }
  python3 tools/frame_dump_diff.py "$steps" "$frames" - "$BASE" "$CAND" \
    | tee "$OUT/frame-diff-$RUN_LABEL-$steps.txt"
done
printf 'verification complete steps=%s end=%s\n' "$STEPS" "$(date +%s.%N)" >> "$OUT/timeline-$RUN_LABEL.log"
cat "$CODE_CHECK" "$CPU_REPORT"
for steps in 2000000 5000000 10000000 20000000; do
  printf 'frame_budget=%s ' "$steps"
  grep 'FRAME IDENTITY:' "$OUT/frame-diff-$RUN_LABEL-$steps.txt"
done
printf 'audio_analysis=%s\nspectrogram=%s\n' "$OUT/$CANDIDATE_LABEL/audio-analysis.txt" "$OUT/$CANDIDATE_LABEL/audio-spec.png"
