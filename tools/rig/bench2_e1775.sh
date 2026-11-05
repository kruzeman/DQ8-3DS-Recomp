#!/usr/bin/env bash
# hotreg/bench2-e1775.sh -- queued behind final2-e1775:
#   (1) two more interleaved A/B rounds (b54 vs e1775), city+field, 20 s each,
#   (2) DET identity vs the base on city+field with the fixed handshake.
set -u
cd "$HOME/DQ8" || exit 2
OUT="$HOME/hotreg/bench2-e1775"
mkdir -p "$OUT"
while systemctl --user is-active --quiet dq8-final2-e1775; do sleep 10; done
bash "$HOME/hotreg/bench-e1773.sh" 2 20 city field -- dq8-b54 dq8-e1775 \
  > "$OUT/bench.txt" 2>&1
DET_OUT="$OUT/det" bash "$HOME/hotreg/vspar_det.sh" "$HOME/DQ8/dq8-e1775" fix8b 8 8000000 city field \
  > "$OUT/det.txt" 2>&1
echo DONE >> "$OUT/bench.txt"
