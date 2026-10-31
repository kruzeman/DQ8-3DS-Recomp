#!/usr/bin/env bash
# Regenerate the DEFAULT translated C (no S1 flags at all) with the tree's own
# dq8tool, using exactly the argument set tools/build_split.sh uses, so two
# trees (baseline vs rollout) can be compared byte-for-byte.
#
# usage: tools/s1_default_regen.sh <output.c>
set -euo pipefail
# Run from the repository root that owns the tree under test (worktree-safe).

OUT=${1:?usage: s1_default_regen.sh <output.c>}
CODE=${DQ8_CODE:-DQ8.code}

args=()
while IFS= read -r address; do
  [[ -z "$address" || "$address" == \#* ]] || args+=(--extra-entry "$address")
done < recomp-roots.txt
while IFS= read -r address; do
  [[ -z "$address" || "$address" == \#* ]] || args+=(--noreturn-call "$address")
done < recomp-noreturn.txt

target/release/dq8tool recompile-arm "$CODE" 0x00100000 "$OUT" "${args[@]}"
echo "generated $OUT"
sha256sum "$OUT"
