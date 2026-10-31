#!/usr/bin/env bash
# Profile the fixed benchmark segment with callgrind, then group the result
# into audit zones (tools/bench_zones.py).
#
# usage: tools/bench_profile.sh [--out FILE] [--pre SECONDS] [--window SECONDS]
#                               [--steps N] [--no-snapshot] [--keep-running]
#
# Why a window: the full 100M-instruction segment takes ~90 s natively, which
# callgrind would stretch into hours (the runner is a -O0 interpreter; valgrind
# instruments every host instruction).  Instrumentation is therefore started
# only after --pre seconds (default 20, i.e. once the run has passed the early
# boot phase) and collected for --window seconds (default 60), which is
# representative of the phase that actually owns the wall time.  Streaming is
# controlled through callgrind_control, so the pre-window part runs at
# near-native speed.
#
# Environment:
#   BUILD=1            build target/dq8-startup-split first if it is missing
#   DQ8_BENCH_NO_SNAPSHOT=1 (or --no-snapshot) additionally suppresses the
#                      diagnostic /tmp PPM screenshots (see tools/bench.sh)
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

OUT=target/callgrind-bench.out
PRE=20
WIN=60
STEPS=100000000
NOSNAP=0
BIN=${BIN:-target/dq8-startup-split}
CODE=${DQ8_CODE:-DQ8.code}
ROMFS=${DQ8_ROMFS:-target/dq8.romfs}
KEEP=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out) OUT=$2; shift 2;;
    --pre) PRE=$2; shift 2;;
    --window) WIN=$2; shift 2;;
    --steps) STEPS=$2; shift 2;;
    --no-snapshot) NOSNAP=1; shift;;
    --keep-running) KEEP=1; shift;;
    -h|--help) sed -n '2,20p' "$0"; exit 0;;
    *) echo "unknown argument: $1" >&2; exit 2;;
  esac
done

if [[ ! -x $BIN ]]; then
  echo "missing $BIN — build it with: DQ8_CODE=$CODE tools/build_split.sh" >&2
  exit 2
fi
[[ -f $CODE ]] || { echo "missing $CODE (set DQ8_CODE)" >&2; exit 2; }
[[ -f $ROMFS ]] || { echo "missing $ROMFS (set DQ8_ROMFS)" >&2; exit 2; }
command -v callgrind_control >/dev/null || { echo "callgrind_control not found" >&2; exit 2; }

rm -f "$OUT" "$OUT".* /tmp/dq8-bench-profile.err
env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 \
    DQ8_BENCH_STEPS=$STEPS DQ8_BENCH_NO_SNAPSHOT=$NOSNAP \
  valgrind --tool=callgrind --callgrind-out-file="$OUT" --instr-atstart=no \
    --cache-sim=no --branch-sim=no \
    "$BIN" "$CODE" "$ROMFS" >/dev/null 2>/tmp/dq8-bench-profile.err &
PID=$!

sleep "$PRE"
echo "== collection ON (pid $PID, steps=$STEPS, no_snapshot=$NOSNAP) =="
callgrind_control -i on >/dev/null 2>&1
sleep "$WIN"
echo "== dump =="
callgrind_control -d >/dev/null 2>&1
callgrind_control -i off >/dev/null 2>&1
if [[ $KEEP -eq 0 ]]; then
  kill "$PID" 2>/dev/null
  sleep 2
  kill -9 "$PID" 2>/dev/null
fi

PROFILE=$(ls -1 "$OUT" "$OUT".* 2>/dev/null | sort | tail -1)
[[ -f ${PROFILE:-} ]] || { echo "no profile written" >&2; exit 1; }
echo "profile: $PROFILE ($(stat -c%s "$PROFILE") bytes)"
grep -m1 '^BENCH' /tmp/dq8-bench-profile.err || true
echo
python3 tools/bench_zones.py "$PROFILE"
