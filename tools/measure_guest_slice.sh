#!/usr/bin/env bash
# Measurements for the preemptive guest slice (t_353154d7).
#
#   tools/measure_guest_slice.sh <base-binary> <new-binary> [seconds]
#
# Sections
#   A  operator savestate (guest list self-cycle at 0x185FF4): 30 s per arm,
#      interleaved base / new / new+DQ8_GUEST_SLICE_MS=0.  Frames, fps over
#      the measured window, mean guest_ms, slice counters and the spinning
#      worker's retired-instruction delta.
#   B  field and battle scenes: 3 interleaved 15 s runs per arm, mean guest_ms
#      per frame (FRAMEPROF), base vs new.
#   C  deterministic run (DQ8_DETERMINISTIC=1 DQ8_BENCH_STEPS=2000000
#      DQ8_TRACE_GPU=1 DQ8_RUN_GUEST_THREADS=1): DET_FRAME md5 and a digest of
#      the PICA trace, slice on vs off.
#
# Everything lands under /tmp/t353/meas and a summary is printed and stored in
# /tmp/t353/meas/summary.txt.
set -u
BASE=${1:?usage: measure_guest_slice.sh <base-bin> <new-bin> [seconds]}
NEW=${2:?usage: measure_guest_slice.sh <base-bin> <new-bin> [seconds]}
SECS=${3:-30}
cd "$(dirname "${BASH_SOURCE[0]}")/.."
OUT=/tmp/t353/meas
mkdir -p "$OUT" "$OUT/ud"
CODE=./DQ8.code
ROMFS=./dq8.romfs
HANG=/home/hermes/DQ8/evidence/hang-20261103/dq8-state.sav
FIELD=/home/hermes/DQ8/evidence/rx580-holes-20261101/scene-holes.sav
BATTLE=/home/hermes/DQ8/evidence/battle-escape-20261101/battle.sav
S="$OUT/summary.txt"
say() { printf '%s\n' "$*" | tee -a "$S"; }
: > "$S"

mean_guest_ms() {  # <log> [first-frame-threshold]
  grep '^FRAMEPROF f=' "$1" | sed -n 's/.* guest_ms=\([0-9.]*\).*/\1/p' \
    | awk -v f="${2:-60}" 'NR>f {s+=$1; n++} END {if (n) printf "%.3f", s/n; else print "NA"}'
}
frame_count() { grep -c '^FRAMEPROF f=' "$1"; }
window_fps() {  # fps over the measured window from FRAMEPROF timestamps
  local log=$1
  local t0 t1 n
  t0=$(grep '^FRAMEPROF f=' "$log" | sed -n '1p' | sed -n 's/.* t=\([0-9.]*\).*/\1/p')
  t1=$(grep '^FRAMEPROF f=' "$log" | tail -1 | sed -n 's/.* t=\([0-9.]*\).*/\1/p')
  n=$(frame_count "$log")
  awk -v a="$t0" -v b="$t1" -v n="$n" 'BEGIN {if (b>a) printf "%.2f", (n-1)/(b-a); else print "NA"}'
}
last_retired() {  # thread id
  grep "guest thread\\[.*id=$1 .*retired=" "$2" | tail -1 | sed 's/.*retired=\([0-9]*\).*/\1/'
}
slice_line() { grep '^SLICE ' "$1" | tail -1; }

run_hang() {  # tag bin extra env...
  local tag=$1 bin=$2; shift 2
  local log="$OUT/hang-$tag.log"
  env DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_FRAMEPROF=1 \
      DQ8_PROGRESS_SECONDS=1 DQ8_SLICE_STATS=1 DQ8_USER_DATA_DIR="$OUT/ud" "$@" \
      timeout -s KILL "$SECS" "$bin" "$CODE" "$ROMFS" --load-state "$HANG" \
      >/dev/null 2>"$log"
  local f fps g t4a t4b d4
  f=$(frame_count "$log"); fps=$(window_fps "$log"); g=$(mean_guest_ms "$log" 60)
  t4a=$(last_retired 00050004 "$log"); t4b=$(grep -o 'thread\[4\].*retired=[0-9]*' "$log" | sed -n '1p' | sed 's/.*retired=//')
  d4=$(grep '^SLICE ' "$log" | tail -1 | sed 's/.*preempts=\([0-9]*\).*/\1/')
  printf '%-18s frames=%-5s fps=%-6s mean_guest_ms=%-8s spinner_retired=%s..%s %s\n' \
    "$tag" "$f" "$fps" "$g" "${t4b:-?}" "${t4a:-?}" "$(slice_line "$log")" | tee -a "$S"
}

say "== A: operator savestate (guest self-cycle at 0x185FF4), ${SECS} s per run, interleaved =="
for r in 1 2; do
  say "-- round $r"
  run_hang "base-r$r" "$BASE"
  run_hang "new-r$r"  "$NEW"
  run_hang "newoff-r$r" "$NEW" DQ8_GUEST_SLICE_MS=0
done

say ""
say "== B: field/battle scenes, 15 s x 3 interleaved, mean guest_ms per frame (frames > 60) =="
run_scene() {  # tag bin state
  local tag=$1 bin=$2 state=$3
  local log="$OUT/scene-$tag.log"
  env DQ8_HEADLESS=1 DQ8_GL=1 DQ8_GL_TEXCACHE=1 DQ8_RUN_GUEST_THREADS=1 DQ8_S1_CFG=all \
      DQ8_VSPAR=4 DQ8_FRAMEPROF=1 DQ8_USER_DATA_DIR="$OUT/ud" \
      timeout -s KILL 15 "$bin" "$CODE" "$ROMFS" --load-state "$state" \
      >/dev/null 2>"$log"
  printf '%-22s frames=%-5s mean_guest_ms=%s\n' "$tag" "$(frame_count "$log")" "$(mean_guest_ms "$log" 60)" | tee -a "$S"
}
for r in 1 2 3; do
  for scene in field battle; do
    state=$FIELD; [ "$scene" = battle ] && state=$BATTLE
    for arm in base new; do
      bin=$BASE; [ "$arm" = new ] && bin=$NEW
      run_scene "$scene-$arm-r$r" "$bin" "$state"
    done
  done
done

say ""
say "== C: deterministic runs (DQ8_TRACE_GPU=1, DQ8_RUN_GUEST_THREADS=1), slice on vs off =="
run_det() {  # tag steps extra env...
  local tag=$1 steps=$2; shift 2
  local log="$OUT/det-$tag.log" dir="$OUT/ud-$tag"
  mkdir -p "$dir"
  env DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_DETERMINISTIC=1 \
      DQ8_BENCH_STEPS="$steps" DQ8_TRACE_GPU=1 \
      DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$dir" DQ8_USER_DATA_DIR="$dir" "$@" \
      timeout -s KILL 900 "$NEW" "$CODE" "$ROMFS" --load-state "$FIELD" \
      >/dev/null 2>"$log"
  local det pica
  det=$(grep -m1 '^DET_FRAME' "$log" | sed 's/ path=.*//')
  pica=$(grep '^PICA ' "$log" | sha256sum | cut -d' ' -f1)
  printf '%-26s %s pica_sha256=%s lines=%s\n' "$tag" "$det" "$pica" "$(grep -c '^PICA ' "$log")" | tee -a "$S"
  printf '%s' "$det" > "$OUT/det-$tag.frame"
  printf '%s' "$pica" > "$OUT/det-$tag.pica"
}
# The card names DQ8_BENCH_STEPS=2000000.  At 2M main instructions no display
# transfer has happened yet in the field state, so DET_FRAME is "missing" in
# both arms; the PICA trace is still compared.  The second pair (30M steps)
# produces a real captured frame, so the md5 comparison is not vacuous.
run_det det2m-on  2000000
run_det det2m-off 2000000 DQ8_GUEST_SLICE_MS=0
run_det det30m-on  30000000
run_det det30m-off 30000000 DQ8_GUEST_SLICE_MS=0
for p in det2m det30m; do
  if cmp -s "$OUT/det-$p-on.frame" "$OUT/det-$p-off.frame" && cmp -s "$OUT/det-$p-on.pica" "$OUT/det-$p-off.pica"; then
    say "$p: slice on == slice off (DET_FRAME and PICA trace)"
  else
    say "$p: DIFFERS between slice on and off"
  fi
done
say ""
say "done: $(date -u +%H:%M:%S)  summary=$S"
