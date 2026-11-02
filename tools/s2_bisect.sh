#!/usr/bin/env bash
# t_6edf5d17: divergence bisect - run both arms to the same fixed guest step
# budget and print the BENCH line (pc at the budget).  Same pc at the same
# budget == same guest trajectory up to that point (the bench line also carries
# main/worker step split).
# usage: tools/s2_bisect.sh <binA> <binB> <steps> [steps...]
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
A=${1:?binA}; B=${2:?binB}; shift 2
for steps in "$@"; do
  for bin in "$A" "$B"; do
    lab="bisect-$(basename "$bin")-$steps"
    out="evidence/t_6edf5d17/bisect/$lab"
    mkdir -p "$out/userdata/archive-00000004-00000000"
    cp -f /home/hermes/DQ8/saves/honeySave.bin \
          "$out/userdata/archive-00000004-00000000/honeySave.bin"
    printf '0\n' > "$out/input.txt"
    env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
        DQ8_FRAME_STATS=0 DQ8_TRACE_TIME=0 DQ8_GL=1 \
        DQ8_BENCH_STEPS="$steps" DQ8_BENCH_NO_SNAPSHOT=1 \
        DQ8_USER_DATA_DIR="$out/userdata" DQ8_INPUT_FILE="$out/input.txt" \
        "$bin" /home/hermes/DQ8/DQ8.code /home/hermes/DQ8/target/dq8.romfs \
        --load-state /home/hermes/DQ8/saves/dq8-state.sav > "$out/run.log" 2>&1 || true
    printf '%-6s %-22s %s\n' "$steps" "$(basename "$bin")" \
      "$(grep -F 'BENCH steps=' "$out/run.log" | tail -1 | cut -c1-160)"
  done
done
