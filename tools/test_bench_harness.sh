#!/usr/bin/env bash
# Contract tests for the §5.1 benchmark harness (tools/bench.sh hooks).
#
#   tools/test_bench_harness.sh
#
# Checks the three properties the perf baseline depends on:
#   1. DQ8_BENCH_STEPS=N stops the run at the step budget and prints exactly one
#      BENCH line with steps / pc / wall / instr_per_sec / snapshots;
#   2. the budget is honoured within one split batch (the counter advances per
#      dispatched batch, so the overshoot is >=1 instruction and small);
#   3. DQ8_BENCH_NO_SNAPSHOT=1 suppresses the /tmp PPM snapshots while the
#      default (variable unset/0) still writes them — i.e. the default runtime
#      behaviour is unchanged by the harness;
#   4. with DQ8_BENCH_STEPS unset the runner does NOT exit at any budget (no
#      BENCH line, keeps running until killed), i.e. normal runs are untouched.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
BIN=${BIN:-target/dq8-startup-split}
CODE=${DQ8_CODE:-DQ8.code}
ROMFS=${DQ8_ROMFS:-target/dq8.romfs}
fail=0

ok()   { echo "  PASS  $*"; }
bad()  { echo "  FAIL  $*"; fail=$((fail + 1)); }

[[ -x $BIN ]] || { echo "missing $BIN" >&2; exit 2; }
[[ -f $CODE ]] || { echo "missing $CODE (set DQ8_CODE)" >&2; exit 2; }
[[ -f $ROMFS ]] || { echo "missing $ROMFS (set DQ8_ROMFS)" >&2; exit 2; }

run() { # run <steps> <no_snapshot> <stderr-file> [extra env assignments...]
  local steps=$1 nosnap=$2 err=$3; shift 3
  env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 \
      DQ8_BENCH_STEPS=$steps DQ8_BENCH_NO_SNAPSHOT=$nosnap "$@" \
      "$BIN" "$CODE" "$ROMFS" >/dev/null 2>"$err"
}

echo "1. BENCH line contract (budget 2,000,000)"
# The runtime emits exactly one aggregate line of the form
#   BENCH steps=... aggregate_steps=...
# plus one BENCH_THREAD line per guest context *as separate evidence*.  The
# aggregate contract is therefore counted with the '^BENCH steps=' prefix so
# the per-thread lines are not mistaken for duplicate aggregate reports.
err=$(mktemp); run 2000000 0 "$err"; rc=$?
line=$(grep -c '^BENCH steps=' "$err")
[[ $rc -eq 0 ]] && ok "exit code 0" || bad "exit code $rc"
[[ $line -eq 1 ]] && ok "exactly one BENCH line" || bad "$line BENCH lines"
b=$(grep -m1 '^BENCH steps=' "$err")
for key in steps pc wall instr_per_sec snapshots no_snapshot \
           main_steps worker_steps aggregate_steps aggregate_instr_per_sec \
           main_dispatches worker_dispatches dispatch_calls \
           idle_main_sleeps idle_worker_sleeps idle_main_wait_spins idle_worker_parks \
           host_yields host_sleeps scene_movie_entries scene_y2r_conversions; do
  grep -q " $key=" <<<"$b" && ok "field $key present" || bad "field $key missing"
done
steps=$(sed -n 's/.*steps=\([0-9]*\).*/\1/p' <<<"$b")
if [[ $steps -ge 2000000 && $steps -le 2100000 ]]; then
  ok "budget honoured (steps=$steps, overshoot $((steps - 2000000)))"
else
  bad "steps=$steps outside [2000000, 2100000]"
fi
rate=$(sed -n 's/.* instr_per_sec=\([0-9.]*\)M.*/\1/p' <<<"$b")
awk -v r="$rate" 'BEGIN {exit !(r > 0.5)}' && ok "instr_per_sec=$rate M is plausible" || bad "instr_per_sec=$rate M"
main=$(sed -n 's/.* main_steps=\([0-9]*\).*/\1/p' <<<"$b")
work=$(sed -n 's/.* worker_steps=\([0-9]*\).*/\1/p' <<<"$b")
agg=$(sed -n 's/.* aggregate_steps=\([0-9]*\).*/\1/p' <<<"$b")
[[ $((main + work)) -eq $agg ]] && ok "aggregate = main + worker ($agg = $main + $work)" \
                                || bad "aggregate $agg != main $main + worker $work"
dmain=$(sed -n 's/.* main_dispatches=\([0-9]*\).*/\1/p' <<<"$b")
dwork=$(sed -n 's/.* worker_dispatches=\([0-9]*\).*/\1/p' <<<"$b")
dcall=$(sed -n 's/.* dispatch_calls=\([0-9]*\).*/\1/p' <<<"$b")
[[ $((dmain + dwork)) -eq $dcall ]] && ok "dispatch_calls = main + worker ($dcall)" \
                                     || bad "dispatch_calls $dcall != $dmain + $dwork"
# Per-thread totals must reconcile with the aggregate line.  guest_threads[]
# holds only threads the guest created, so the BENCH_THREAD rows sum to
# worker_steps; the main context is the host-side dispatch loop and is reported
# by the main_steps field.
tcount=$(grep -c '^BENCH_THREAD ' "$err")
[[ $tcount -ge 1 ]] && ok "$tcount BENCH_THREAD line(s) emitted" || bad "no BENCH_THREAD lines"
tsum=$(grep '^BENCH_THREAD ' "$err" | sed -n 's/.* retired=\([0-9]*\).*/\1/p' | awk '{s+=$1} END {print s+0}')
[[ ${tsum:-0} -eq ${work:-1} ]] && ok "per-thread retired sum = worker_steps ($tsum)" \
                               || bad "per-thread retired sum $tsum != worker_steps $work"
grep -q '268e6\|9.38e9/(r\*1e6)' tools/bench.sh \
  && bad "bench.sh still converts MHz to instructions/FPS" \
  || ok "bench.sh has no MHz->instructions->FPS projection"
rm -f "$err"

echo "2. snapshot switch (budget 20,000,000; DQ8_CAPTURE_PPM=1 so that the"
echo "   opt-in producer exists on main @ 38b1ce9 as well as on fc7d61d)"
err0=$(mktemp); run 20000000 0 "$err0" DQ8_CAPTURE_PPM=1
s0=$(sed -n 's/.*snapshots=\([0-9]*\).*/\1/p' <<<"$(grep -m1 '^BENCH steps=' "$err0")")
err1=$(mktemp); run 20000000 1 "$err1" DQ8_CAPTURE_PPM=1
s1=$(sed -n 's/.*snapshots=\([0-9]*\).*/\1/p' <<<"$(grep -m1 '^BENCH steps=' "$err1")")
[[ ${s0:-0} -gt 0 ]] && ok "producer enabled -> snapshots written (snapshots=$s0)" \
                     || bad "no snapshots even with DQ8_CAPTURE_PPM=1"
[[ ${s1:-1} -eq 0 ]] && ok "DQ8_BENCH_NO_SNAPSHOT=1 suppresses snapshots" \
                     || bad "snapshots=$s1 with the switch on"
ns=$(sed -n 's/.*no_snapshot=\([01]\).*/\1/p' <<<"$(grep -m1 '^BENCH steps=' "$err1")")
[[ $ns == 1 ]] && ok "no_snapshot flag reported" || bad "no_snapshot flag not reported"
rm -f "$err0" "$err1"

echo "3. unset DQ8_BENCH_STEPS keeps normal behaviour (7 s probe)"
err=$(mktemp)
env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_PROGRESS_SECONDS=1 \
    "$BIN" "$CODE" "$ROMFS" >/dev/null 2>"$err" &
probe=$!
sleep 7
kill -9 "$probe" 2>/dev/null
wait "$probe" 2>/dev/null
[[ $(grep -c '^BENCH steps=' "$err") -eq 0 ]] && ok "no BENCH line without the budget" \
                                     || bad "BENCH line appeared without DQ8_BENCH_STEPS"
grep -q 'guest snapshot pc=' "$err" && ok "guest kept running (progress dump seen)" \
                                   || bad "no progress dump — run did not behave normally"
rm -f "$err"

echo
if [[ $fail -eq 0 ]]; then echo "ALL CHECKS PASSED"; exit 0; fi
echo "$fail CHECK(S) FAILED"; exit 1
