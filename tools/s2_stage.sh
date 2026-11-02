#!/usr/bin/env bash
# t_6edf5d17: generate + build one S2 stage variant from the accepted
# target/dq8-startup.c (no recompiler re-run: every arm must see the same
# generated guest C so only the dispatch transform differs).
# usage: tools/s2_stage.sh <out-binary> <split-dir> <chain|entry|cold|block-mode>
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
OUT=${1:?out binary}
SPLIT=${2:?split dir}
MODE=${3:-chain}
mkdir -p "$SPLIT"
echo "== S2 generate $(date -Is) mode=$MODE -> $SPLIT"
if [[ $MODE == chain ]]; then
  env -u DQ8_S2 DQ8_SPLIT_CHAIN=1 python3 tools/split_dispatch.py target/dq8-startup.c "$SPLIT" \
      --overrides tools/overrides/registry.json
else
  DQ8_SPLIT_CHAIN=1 DQ8_S2="$MODE" python3 tools/split_dispatch.py target/dq8-startup.c "$SPLIT" \
      --overrides tools/overrides/registry.json
fi
du -sh "$SPLIT"
bash tools/s2_build.sh "$OUT" "$SPLIT" "$MODE"
