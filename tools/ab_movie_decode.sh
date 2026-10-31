#!/usr/bin/env bash
# Seeded movie decode-throughput A/B (task t_c6ea81c4). One configuration per
# invocation: AB_BIN=<binary> AB_TAG=<tag> AB_S1=<0|all> AB_ENV='<extra env>' \
#   tools/ab_movie_decode.sh
# Headless, prepared save, DQ8_BENCH_STEPS-bound; reports wall + y2r conversions.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
SCRATCH=${SCRATCH:-/home/hermes/.hermes/profiles/renderer/cache/scratch}
BIN=${AB_BIN:-target/dq8-movie-trace}
TAG=${AB_TAG:-run}
S1=${AB_S1:-0}
STEPS=${AB_STEPS:-40000000}
UD=$SCRATCH/ab-ud-$TAG
cp -r /home/hermes/DQ8/target/userdata-title-1 "$UD"
echo 0 > "$SCRATCH/ab-input.txt"
ENV_ARGS=(DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_BENCH_STEPS=$STEPS
          DQ8_BENCH_NO_SNAPSHOT=1 DQ8_USER_DATA_DIR=$UD
          DQ8_INPUT_FILE=$SCRATCH/ab-input.txt)
if [ "$S1" != "0" ]; then
  ENV_ARGS+=(DQ8_S1_CFG=$S1)
fi
# Extra environment (documented in the header; was previously dropped here).
# Space-separated VAR=VALUE tokens, appended after the fixed set so a caller
# can flip gates like DQ8_DISABLE_OVERRIDES per configuration.
if [ -n "${AB_ENV:-}" ]; then
  read -r -a _ab_extra <<< "$AB_ENV"
  ENV_ARGS+=("${_ab_extra[@]}")
fi
echo "=== AB run tag=$TAG bin=$BIN s1=$S1 steps=$STEPS ==="
t0=$(date +%s.%N)
env "${ENV_ARGS[@]}" timeout 300 "$BIN" /home/hermes/DQ8/DQ8.code /home/hermes/DQ8/target/dq8.romfs \
  > "$SCRATCH/ab-$TAG.log" 2>&1 || true
t1=$(date +%s.%N)
echo "wall_total=$(awk "BEGIN{print $t1 - $t0}")"
grep -E "^BENCH " "$SCRATCH/ab-$TAG.log" | tail -1
echo "log=$SCRATCH/ab-$TAG.log"
