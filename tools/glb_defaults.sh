#!/usr/bin/env bash
# t_9651f10a: default-config gates check — run the binary with NO DQ8_GL_* tuning
# env (DQ8_GL=1 only selects the backend) at the operator's save-menu savestate
# and record the GL backend decision lines + the GLPHASE counters.
#
# usage: tools/glb_defaults.sh <label> <outdir> <binary> [ENV=VAL ...]
set -u
LABEL=${1:?label}; OUT=${2:?outdir}; BIN=${3:?binary}; shift 3
EXTRA=("$@")
SECS=${SECS:-20}
W=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
STATE=${STATE:-/home/hermes/DQ8/saves/dq8-state.sav}
UD=${UD:-/home/hermes/.hermes/profiles/coder/cache/scratch/glb_defaults_ud}
mkdir -p "$OUT" "$UD"
# Belt and braces: never let an inherited autosave knob touch the shared state.
unset DQ8_SAVESTATE_AUTOSAVE_AT
cd "$W"
echo "== $LABEL $(date -Is) bin=$BIN extra=${EXTRA[*]:-none} =="
env DQ8_HEADLESS=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT" \
    DQ8_TRACE_TIME=1 DQ8_RUN_GUEST_THREADS=1 DQ8_USER_DATA_DIR="$UD" \
    DQ8_GL=1 "${EXTRA[@]}" \
    "$BIN" DQ8.code dq8.romfs --load-state "$STATE" > "$OUT/run.log" 2>&1 &
GP=$!
sleep "$SECS"
kill -9 $GP 2>/dev/null; wait $GP 2>/dev/null
echo "---- decision lines ($LABEL)"
grep -m1 "GL backend: fixed-function blending" "$OUT/run.log"
grep -m1 "fixed-function blend depth guard" "$OUT/run.log"
grep -m1 "framebuffer fetch" "$OUT/run.log"
grep -m1 "GL backend: renderer" "$OUT/run.log"
echo "---- last GLPHASE ($LABEL)"
grep "GLPHASE" "$OUT/run.log" | tail -1
grep -c "GLPHASE" "$OUT/run.log" | sed 's/^/GLPHASE lines: /'
ls -l "$OUT"/dq8-screen.ppm 2>/dev/null | sed 's/^/frame: /'
