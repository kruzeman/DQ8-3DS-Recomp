#!/usr/bin/env bash
# Reproducible DQ8 performance benchmark (audit 5.1, hardened by PERF-02).
#
#   tools/bench.sh [--steps N] [--repeat R] [--no-snapshot] [--log FILE]
#                  [--expect-scene movie|any]
#
# Fixed segment definition: the first N *main-context* guest instructions
# executed from the reset entry point (0x00100000), counted with the guest
# instruction counter that the split-batch accounting already maintains (A05).
# The runner stops exactly at that budget and prints one BENCH line.
#
# PERF-02 (honest counters): the budget is a MAIN-context budget, not a
# budget of all guest instructions — workers keep running while the main
# context executes, and their retirement is reported separately:
#
#   main_steps / worker_steps / aggregate_steps   retired instructions
#   main_dispatches / worker_dispatches           translated_from calls
#   idle_main_sleeps / idle_worker_sleeps         SleepThread park calls
#   idle_main_wait_spins / idle_worker_parks      wait-service iterations
#   host_yields / host_sleeps                     host sched_yield/nanosleep
#   scene_movie_entries / scene_y2r_conversions   what the segment reached
#
# The aggregate rate is the only guest-throughput number this script reports.
# There is NO MHz -> instructions -> FPS / "projected intro wall time"
# conversion: the 268 MHz core clock is a reference frequency, not a retired
# instruction count, and no measured cycles/CPI reference exists yet.
#
# Scene verdict: a run that never enters the movie player (0x002FA318) and
# never completes a Y2R conversion did not reach the intro — with a clean save
# that is the first-run save modal.  The summary flags it explicitly instead of
# presenting the segment as intro progress; --expect-scene movie turns the flag
# into a hard failure (exit 3) for scene-sensitive comparisons.
#
# Defaults: N = 100000000 (100M main instructions), R = 3 runs, snapshots ON
# (as-shipped runtime).  --no-snapshot adds DQ8_BENCH_NO_SNAPSHOT=1.
#
# Every run is headless with guest threads enabled (the audit configuration):
# DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

STEPS=100000000
REPEAT=3
NOSNAP=0
BIN=${BIN:-target/dq8-startup-split}
CODE=${DQ8_CODE:-DQ8.code}
ROMFS=${DQ8_ROMFS:-target/dq8.romfs}
LOG=${DQ8_BENCH_LOG:-}
EXPECT_SCENE=any

while [[ $# -gt 0 ]]; do
  case "$1" in
    --steps) STEPS=$2; shift 2;;
    --repeat) REPEAT=$2; shift 2;;
    --no-snapshot) NOSNAP=1; shift;;
    --log) LOG=$2; shift 2;;
    --binary) BIN=$2; shift 2;;
    --expect-scene) EXPECT_SCENE=$2; shift 2;;
    -h|--help) sed -n '2,45p' "$0"; exit 0;;
    *) echo "unknown argument: $1" >&2; exit 2;;
  esac
done

[[ -f $CODE ]] || { echo "missing $CODE (set DQ8_CODE or symlink DQ8.code)" >&2; exit 2; }
[[ -f $ROMFS ]] || { echo "missing $ROMFS (set DQ8_ROMFS or symlink target/dq8.romfs)" >&2; exit 2; }
if [[ ! -x $BIN ]]; then
  echo "building $BIN (missing)..." >&2
  DQ8_CODE=$CODE BUILD_JOBS=${BUILD_JOBS:-6} tools/build_split.sh "$BIN" >&2 || exit 2
fi
if [[ -z $LOG ]]; then
  mkdir -p reports
  LOG=reports/bench-$(date -u +%Y%m%dT%H%M%SZ).log
fi
mkdir -p "$(dirname "$LOG")"

say() { printf '%s\n' "$*" | tee -a "$LOG"; }

field() { sed -n "s/.* $2=\([0-9A-Fa-f.]*\).*/\1/p" <<<"$1"; }

say "DQ8 perf benchmark (audit 5.1, PERF-02 honest counters)"
say "date:      $(date -u +%Y-%m-%dT%H:%M:%SZ)  host: $(uname -m)  cpus: $(nproc)"
say "binary:    $BIN ($(stat -c '%s bytes, %y' "$BIN" 2>/dev/null | cut -d. -f1))"
say "segment:   first $STEPS MAIN-context guest instructions from reset (0x00100000)"
say "config:    DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_BENCH_NO_SNAPSHOT=$NOSNAP"
say "runs:      $REPEAT   expect-scene: $EXPECT_SCENE"
say ""

agg_list=()
main_list=()
wall_list=()
scene_fail=0
for ((i = 1; i <= REPEAT; ++i)); do
  err=$(mktemp)
  out=$(env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 \
            DQ8_BENCH_STEPS=$STEPS DQ8_BENCH_NO_SNAPSHOT=$NOSNAP \
            "$BIN" "$CODE" "$ROMFS" 2>"$err" >/dev/null)
  line=$(grep -m1 '^BENCH ' "$err")
  if [[ -z $line ]]; then
    say "run $i: FAILED (no BENCH line; last log line: $(tail -n1 "$err"))"
    rm -f "$err"
    exit 1
  fi
  wall=$(field "$line" wall)
  rate=$(field "$line" instr_per_sec)
  arate=$(field "$line" aggregate_instr_per_sec)
  main=$(field "$line" main_steps)
  work=$(field "$line" worker_steps)
  agg=$(field "$line" aggregate_steps)
  disp=$(field "$line" dispatch_calls)
  snap=$(field "$line" snapshots)
  pc=$(field "$line" pc)
  movie=$(field "$line" scene_movie_entries)
  y2r=$(field "$line" scene_y2r_conversions)
  say "run $i:     main=$main worker=$work aggregate=$agg pc=$pc wall=$wall s"
  say "            aggregate=$arate M/s main=$rate M/s dispatches=$disp snapshots=$snap"
  say "            idle: main_sleeps=$(field "$line" idle_main_sleeps) worker_sleeps=$(field "$line" idle_worker_sleeps) main_wait_spins=$(field "$line" idle_main_wait_spins) worker_parks=$(field "$line" idle_worker_parks) host_yields=$(field "$line" host_yields) host_sleeps=$(field "$line" host_sleeps)"
  if [[ ${movie:-0} -eq 0 && ${y2r:-0} -eq 0 ]]; then
    say "            scene: modal/startup (movie_entries=$movie y2r_conversions=$y2r) — NOT the intro"
    scene_fail=$((scene_fail + 1))
  else
    say "            scene: movie_entries=$movie y2r_conversions=$y2r"
  fi
  agg_list+=("$arate")
  main_list+=("$rate")
  wall_list+=("$wall")
  rm -f "$err"
done

stats=$(printf '%s\n' "${agg_list[@]}" | sort -n | awk '
  {v[NR]=$1}
  END {
    n=NR; med=(n%2)?v[(n+1)/2]:(v[n/2]+v[n/2+1])/2
    printf "%.3f %.3f %.3f %.3f", med, v[1], v[n], (v[1]>0 ? (v[n]-v[1])/v[1]*100 : 0)
  }')
amed=$(cut -d' ' -f1 <<<"$stats")
alo=$(cut -d' ' -f2 <<<"$stats")
ahi=$(cut -d' ' -f3 <<<"$stats")
aspread=$(cut -d' ' -f4 <<<"$stats")
mmed=$(printf '%s\n' "${main_list[@]}" | sort -n | awk '{v[NR]=$1} END {n=NR; print (n%2)?v[(n+1)/2]:(v[n/2]+v[n/2+1])/2}')
wstats=$(printf '%s\n' "${wall_list[@]}" | sort -n | awk '{v[NR]=$1} END {n=NR; med=(n%2)?v[(n+1)/2]:(v[n/2]+v[n/2+1])/2; printf "%.3f %.3f %.3f", med, v[1], v[n]}')
wmed=$(cut -d' ' -f1 <<<"$wstats")

say ""
say "summary (median of $REPEAT runs)"
say "  main-context budget:    $STEPS instructions (workers run concurrently)"
say "  wall time:              $wmed s  (min $(cut -d' ' -f2 <<<"$wstats"), max $(cut -d' ' -f3 <<<"$wstats"))"
say "  aggregate throughput:   $amed M retired instr/s  (min $alo, max $ahi, spread $aspread%)"
say "  main-context rate:      $mmed M instr/s"
say ""
say "reference frequency (NOT a throughput conversion)"
say "  268 MHz is the 3DS core clock.  This harness deliberately does not convert"
say "  it into instructions, FPS or a projected intro wall time: retired"
say "  instructions, CPI and wait/idle time are measured separately above, and"
say "  no trace-derived cycles reference exists (PERF-02)."
say ""
if [[ $EXPECT_SCENE == movie && $scene_fail -gt 0 ]]; then
  say "scene verdict: FAIL — $scene_fail/$REPEAT run(s) never reached the intro (modal/startup)"
  say "log: $LOG"
  exit 3
fi
if [[ $scene_fail -gt 0 ]]; then
  say "scene verdict: flagged — $scene_fail/$REPEAT run(s) stayed in modal/startup, not the intro"
else
  say "scene verdict: movie path reached in every run"
fi
say ""
say "log: $LOG"
