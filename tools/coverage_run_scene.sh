#!/usr/bin/env bash
# One bounded dynamic-coverage run: BENCH-mode guest run of an S1-gated split
# binary on an established scene, capturing stderr (BENCH + S1_CFG reporter
# lines).  Measurement only - the binary and the runtime are untouched.
#
# usage: tools/coverage_run_scene.sh <binary> <scene> <bench_steps> \
#          <userdata_src|-fresh-> [press_spec...]
#   press_spec   <second>[:mask]  - write that button mask to DQ8_INPUT_FILE
#                for 1 s at that wall-clock second (mask 1 = A, as in
#                tools/smoke_hid_input.sh / tools/test_startup_title.sh)
#   userdata_src prepared save dir that is COPIED (never written in place),
#                or -fresh- for an empty userdata dir
#
# Environment: DQ8_S1_CFG (region list), DQ8_SKIP_MOVIE, COV_TIMEOUT,
# COV_OUT, DQ8_CODE, DQ8_ROMFS.
set -u
BIN=$(readlink -f "$1"); SCENE=$2; STEPS=$3; UD_SRC=$4; shift 4
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
OUT=${COV_OUT:-$ROOT/target/coverage-runs/$SCENE}
mkdir -p "$OUT"
cd "$OUT"
ln -sf "${DQ8_CODE:-$ROOT/DQ8.code}" DQ8.code
ln -sf "${DQ8_ROMFS:-$ROOT/target/dq8.romfs}" dq8.romfs
UD=$OUT/userdata
rm -rf "$UD"
if [ "$UD_SRC" = "-fresh-" ]; then mkdir -p "$UD"; else cp -a "$UD_SRC" "$UD"; fi
INPUT=$OUT/input.txt
printf '0\n' > "$INPUT"
: > "$OUT/run.err"
# NOTE: only pass optional env when actually set - the runtime tests the
# pointer, so DQ8_PROGRESS_SECONDS="" would enable the periodic dump.
EXTRA=()
[ -n "${DQ8_PROGRESS_SECONDS:-}" ] && EXTRA+=("DQ8_PROGRESS_SECONDS=$DQ8_PROGRESS_SECONDS")
env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_BENCH_NO_SNAPSHOT=1 \
DQ8_USER_DATA_DIR="$UD" DQ8_INPUT_FILE="$INPUT" \
DQ8_S1_CFG="${DQ8_S1_CFG:-}" DQ8_SKIP_MOVIE="${DQ8_SKIP_MOVIE:-}" \
"${EXTRA[@]}" \
DQ8_BENCH_STEPS="$STEPS" \
timeout -s KILL "${COV_TIMEOUT:-420}" "$BIN" DQ8.code dq8.romfs \
  > "$OUT/run.out" 2> "$OUT/run.err" &
pid=$!
now=0
for spec in "$@"; do
  at=${spec%%:*}; mask=${spec#*:}; [ "$mask" = "$spec" ] && mask=1
  delta=$(( at - now ))
  if [ "$delta" -gt 0 ]; then sleep "$delta"; fi
  now=$at
  printf '%s\n' "$mask" > "$INPUT"
  sleep 1
  printf '0\n' > "$INPUT"
done
wait "$pid"; rc=$?
echo "== $SCENE rc=$rc =="
grep -E "^BENCH |^S1_CFG |translated PC escaped|unsupported guest|guest memory fault|invalid handle" \
  "$OUT/run.err" | head -40
exit $rc
