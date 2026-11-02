#!/usr/bin/env bash
# run_hotregions.sh -- one measured scenario run for the S1b hot-region ranking
# (t_f79c2913).
#
# usage: run_hotregions.sh <prof|base> <scenario> <savestate> <seconds> <outdir>
#
#   prof : DQ8_GUEST_PC_PROFILE=1 -- the runtime counts every retired guest
#          instruction per 4 KB page / per PC and prints the raw counters to
#          stderr after <seconds> (DQ8_GUEST_PC_PROFILE_SECS), then exits.
#   base : same binary, counters OFF -- FRAMEPROF only, so the guest time and
#          throughput of the uninstrumented path can be compared with `prof`.
#
# Both modes: headless (no SDL window), DQ8_RUN_GUEST_THREADS=1, GL backend on
# (EGL), per-run empty userdata dir.  The savestate restores the scene, so the
# guest never walks the boot path.
set -u
MODE=${1:?mode prof|base}
NAME=${2:?scenario name}
STATE=${3:?savestate}
SECS=${4:?seconds}
OUT=${5:?outdir}
BIN=${BIN:-target/dq8-hotreg-prof}
CODE=${DQ8_CODE:-DQ8.code}
ROMFS=${DQ8_ROMFS:-target/dq8.romfs}
mkdir -p "$OUT/ud-$MODE-$NAME"

common=(DQ8_HEADLESS=1 DQ8_DISPLAY=0 DQ8_RUN_GUEST_THREADS=1
        DQ8_GL=1 DQ8_FRAMEPROF=1 DQ8_PROGRESS_SECONDS=1
        DQ8_USER_DATA_DIR="$PWD/$OUT/ud-$MODE-$NAME")

if [[ "$MODE" == prof ]]; then
  env "${common[@]}" DQ8_GUEST_PC_PROFILE=1 DQ8_GUEST_PC_PROFILE_SECS="$SECS" \
    stdbuf -oL "$BIN" --load-state "$STATE" "$CODE" "$ROMFS" \
    > "$OUT/prof-$NAME.stdout" 2> "$OUT/prof-$NAME.stderr"
  echo "prof $NAME rc=$? -> $OUT/prof-$NAME.stderr"
else
  timeout -s KILL "$SECS" env "${common[@]}" \
    stdbuf -oL "$BIN" --load-state "$STATE" "$CODE" "$ROMFS" \
    > "$OUT/base-$NAME.stdout" 2> "$OUT/base-$NAME.stderr"
  echo "base $NAME rc=$? -> $OUT/base-$NAME.stderr"
fi
