#!/bin/bash
# t_8df70ad4: one DQ8 run with the perf-audit environment.
#
#   tools/zwb_ab_run.sh <label> <binary> <state.sav> <secs> [VAR=VAL ...]
#
# Runs from $HOME/DQ8 (DQ8.code, dq8.romfs, binary and state paths are relative
# to it), headless EGL, guest thread on, VSPAR=4, split path off, FRAMEPROF and
# DQ8_XFERPROF on.  Extra VAR=VAL arguments are added to the environment — that
# is how the depth write-back A/B is run:
#
#   tools/zwb_ab_run.sh f1 ./dq8-zwb5 saves/scene-holes.sav 20 DQ8_GL_ZWB=1
#   tools/zwb_ab_run.sh f0 ./dq8-zwb5 saves/scene-holes.sav 20 DQ8_GL_ZWB=0
#   python3 tools/zwb_ab_sum.py ZWB=1:/tmp/az-f1 ZWB=0:/tmp/az-f0
#
set -u
cd "$HOME/DQ8" || exit 1
LABEL=$1; BIN=$2; STATE=$3; SECS=$4; shift 4
D=${AZ_D:-/tmp/az-}$LABEL
rm -rf "$D"; mkdir -p "$D"
env DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_CAPTURE_PPM=1 \
    DQ8_CAPTURE_PPM_DIR="$D" DQ8_VSPAR=4 DQ8_SPLIT_GL=0 \
    DQ8_STACK_NO_RELOCATE=1 DQ8_GL_DEPTH_ATTACH=1 DQ8_FRAMEPROF=1 \
    DQ8_XFERPROF=1 "$@" \
    timeout -k 5 "$SECS" "$BIN" --load-state "$STATE" \
        "$HOME/DQ8/DQ8.code" "$HOME/DQ8/dq8.romfs" > "$D/out.log" 2> "$D/err.log"
rc=$?
echo "== $LABEL rc=$rc ppm=$(ls "$D"/*.ppm 2>/dev/null | tr '\n' ' ')"
