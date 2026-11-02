#!/usr/bin/env bash
# t_d559a510 attempt 3: deterministic A/B on the operator's savestate (read-only).
#   sw    : DQ8_GL_BLENDS=0                      (shader/SW blend = reference)
#   gl    : DQ8_GL_BLENDS=1                      (fixed-function, depth guard ON)
#   glbug : DQ8_GL_BLENDS=1 DQ8_GL_BLENDDEPTH=0  (pre-fix: depth also blended)
# One extra arm with frame capture only (no drawhash) so the per-draw readback
# does not perturb the frames:
#   swf / glf : same two configs, DQ8_GL_DRAWHASH unset
set -u
W=/home/hermes/DQ8/worktrees/glblend-t_d559a510
SECS=${SECS:-18}
OUT=${OUT:-$W/evidence/t_d559a510/a3}
BIN=${BIN:-$W/target/dq8-glblend}
STATE=${STATE:-/home/hermes/DQ8/saves/dq8-state.sav}
UD=/home/hermes/.hermes/profiles/coder/cache/scratch/glb_ud_a3
mkdir -p "$UD"
cd "$W"
run() {  # run <name> <extra env...>
  local name=$1; shift
  local d="$OUT/$name"
  mkdir -p "$d"; rm -f "$d"/f_*.ppm "$d"/dq8-screen.ppm
  echo "== $name $(date -Is) : $*"
  env DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$d" \
      DQ8_TRACE_TIME=1 DQ8_RUN_GUEST_THREADS=1 DQ8_USER_DATA_DIR="$UD" \
      DQ8_GL_DRAWHASH_TAG="$name" \
      "$@" "$BIN" DQ8.code dq8.romfs --load-state "$STATE" \
      > "$d/run.log" 2>&1 &
  local gp=$! t0=$(date +%s) last="" n=0
  : > "$d/frames.timeline"
  while [ $(( $(date +%s) - t0 )) -lt "$SECS" ]; do
    f="$d/dq8-screen.ppm"
    if [ -f "$f" ] && [ "$(stat -c %s "$f")" -gt 200000 ]; then
      h=$(md5sum "$f" | cut -d' ' -f1)
      if [ "$h" != "$last" ]; then
        n=$((n+1)); cp "$f" "$(printf '%s/f_%03d_%s.ppm' "$d" "$n" "$h")"; last=$h
        echo "t=$(( $(date +%s) - t0 )) f_$(printf '%03d' $n)_$h.ppm" >> "$d/frames.timeline"
      fi
    fi
    sleep 0.2
  done
  kill -9 $gp 2>/dev/null; wait $gp 2>/dev/null
  echo "$name distinct_frames=$n"
  grep -m1 "GL backend: fixed-function blending" "$d/run.log"
  grep -m1 "framebuffer fetch" "$d/run.log"
  grep -m1 "fixed-function blend depth guard" "$d/run.log"
  grep "GLPHASE" "$d/run.log" | tail -1
}

run sw    DQ8_GL=1 DQ8_GL_BLENDS=0 DQ8_GL_DRAWHASH=1
run gl    DQ8_GL=1 DQ8_GL_BLENDS=1 DQ8_GL_DRAWHASH=1
run glbug DQ8_GL=1 DQ8_GL_BLENDS=1 DQ8_GL_BLENDDEPTH=0 DQ8_GL_DRAWHASH=1
run swf   DQ8_GL=1 DQ8_GL_BLENDS=0
run glf   DQ8_GL=1 DQ8_GL_BLENDS=1
echo "== DONE $(date -Is)"
