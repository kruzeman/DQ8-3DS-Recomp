#!/usr/bin/env bash
# Run the batch-3 per-region differential/cost fixture for every region of the
# batch (or for the entries passed as arguments), one log line-set per region.
#
# usage: tools/s1_r3_fixture.sh [0xENTRY ...]        # all regions by default
#        STRICT=0 tools/s1_r3_fixture.sh ...         # pass --allow-partial
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
OUT=${OUT:-target/s1r3-fixture}
PAR=${PAR:-4}
STRICT=${STRICT:-1}
mkdir -p "$OUT"

if (( $# > 0 )); then
  regions=("$@")
else
  regions=(
    0x001A93A8 0x001A9428 0x001A96C8 0x001A9904 0x001A99A8 0x00194D64 0x00195D44
    0x001A982C 0x001A9A88 0x00196A78 0x00193A78
    0x001A72D4 0x001A78C0 0x001A7BCC 0x001A6EA4 0x001A7068
    0x001A9D70 0x001AA05C 0x001AA1A0 0x001AA3B0 0x001AA4F4 0x001AA6F4 0x001AA828 0x001AA9B8
  )
fi

args=()
(( STRICT )) || args+=(--allow-partial)

pids=()
running=0
for e in "${regions[@]}"; do
  (
    DQ8_RUN_GUEST_THREADS=1 python3 tools/test_s1_cfg_regions_r3.py "${args[@]}" "$e" \
      > "$OUT/$e.log" 2>&1
    echo "$?" > "$OUT/$e.rc"
  ) &
  pids+=($!)
  running=$((running + 1))
  if (( running >= PAR )); then
    wait -n 2>/dev/null || wait
    running=$((running - 1))
  fi
done
wait

pass=0; fail=0
for e in "${regions[@]}"; do
  rc=$(cat "$OUT/$e.rc" 2>/dev/null || echo 99)
  if [[ "$rc" == 0 ]]; then pass=$((pass+1)); else fail=$((fail+1)); fi
  printf '%s rc=%s %s\n' "$e" "$rc" \
    "$(grep -c 'S1R3_STATE_RESULT' "$OUT/$e.log" 2>/dev/null || echo 0) state-results"
done
echo "S1R3_FIXTURE_SUMMARY pass=$pass fail=$fail (logs in $OUT/)"
