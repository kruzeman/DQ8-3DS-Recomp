#!/usr/bin/env bash
# t_3de68379: interleaved A/B of two stage binaries on the savestate scene.
#
# usage: tools/codegen_ab.sh <binA> <binB> <pairs> <dur_s>
#
# The box runs other agents' DQ8 processes (load 15-20 of 24 cores), so a
# single back-to-back comparison of fps is not usable: the arms are run
# INTERLEAVED and the order is alternated inside each pair, exactly like
# tools/s1d_ab.py, so a host-load trend hits both arms.
# Writes evidence/t_3de68379/ab/<tag>-<n>/run.log and a summary table.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

A=${1:?bin A}
B=${2:?bin B}
PAIRS=${3:-3}
DUR=${4:-90}
OUT="$PWD/evidence/t_3de68379/ab"
mkdir -p "$OUT"

run_one() {   # run_one <binary> <label>
  local bin=$1 lab=$2
  local dir="$OUT/$lab"
  mkdir -p "$dir/userdata/archive-00000004-00000000"
  cp -f /home/hermes/DQ8/saves/honeySave.bin \
        "$dir/userdata/archive-00000004-00000000/honeySave.bin"
  printf '0\n' > "$dir/input.txt"
  : > "$dir/run.log"
  env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
      DQ8_FRAME_STATS=1 DQ8_TRACE_TIME=1 DQ8_GL=1 \
      DQ8_USER_DATA_DIR="$dir/userdata" DQ8_INPUT_FILE="$dir/input.txt" \
      "$bin" /home/hermes/DQ8/DQ8.code /home/hermes/DQ8/target/dq8.romfs \
      --load-state /home/hermes/DQ8/saves/dq8-state.sav \
      > "$dir/run.log" 2>&1 &
  local gp=$!
  sleep "$DUR"
  kill -TERM "$gp" 2>/dev/null || true
  sleep 3
  kill -9 "$gp" 2>/dev/null || true
  wait "$gp" 2>/dev/null || true
}

ta=$(basename "$A")
tb=$(basename "$B")
echo "== interleaved A/B: $ta vs $tb, $PAIRS pairs x ${DUR}s =="
for i in $(seq 1 "$PAIRS"); do
  if (( i % 2 == 1 )); then first="$A:$ta"; second="$B:$tb"; else first="$B:$tb"; second="$A:$ta"; fi
  for spec in "$first" "$second"; do
    bin=${spec%%:*}; tag=${spec##*:}
    echo "-- pair $i $tag"
    run_one "$bin" "$tag-$i"
  done
done

python3 tools/codegen_ab_summary.py "$OUT" "$ta" "$tb"
