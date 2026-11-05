#!/usr/bin/env bash
# t_e216fc37: reproduce the black ground quad from the operator savestate.
#
#   bash tools/sq_run.sh <tag> [extra env assignments ...]
#
# Writes evidence/render-shadow-quad/<tag>.log and captures the top screen to
# evidence/render-shadow-quad/cap-<tag>/dq8-screen.ppm.
#
# Environment knobs the caller may override:
#   DQ8_GL=1        hardware (EGL/GL) renderer; default 0 = software rasteriser
#   SQ_STEPS        DQ8_BENCH_STEPS budget (default 20000000)
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.."
tag=${1:?usage: sq_run.sh <tag> [ENV=V ...]}
shift
OUT=evidence/render-shadow-quad
mkdir -p "$OUT/cap-$tag"
export DQ8_HEADLESS=1
export DQ8_GL=${DQ8_GL:-0}
export DQ8_DETERMINISTIC=1
export DQ8_BENCH_STEPS=${SQ_STEPS:-20000000}
export DQ8_CAPTURE_PPM=1
export DQ8_CAPTURE_PPM_DIR=$PWD/$OUT/cap-$tag
export DQ8_RUN_GUEST_THREADS=1
export DQ8_SPLIT_GL=0
export DQ8_VSPAR=8
export DQ8_STACK_NO_RELOCATE=1
export DQ8_PROGRESS_SECONDS=${DQ8_PROGRESS_SECONDS:-}
for kv in "$@"; do export "$kv"; done
# Optional per-run environment file (KEY=VALUE lines); avoids shell quoting of
# values that contain commas/colons when the harness is driven from a wrapper.
if [ -f "$OUT/$tag.env" ]; then
    set -a
    # shellcheck disable=SC1090
    . "$OUT/$tag.env"
    set +a
fi
timeout "${SQ_TIMEOUT:-900}" ./target/dq8-sq \
    --load-state /home/hermes/DQ8/evidence/render-cape-path/dq8-state.sav \
    DQ8.code dq8.romfs >"$OUT/$tag.log" 2>&1
echo "exit=$? (tag=$tag) log=$OUT/$tag.log"
