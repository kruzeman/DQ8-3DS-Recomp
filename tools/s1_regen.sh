#!/usr/bin/env bash
# Regenerate the translated C only (no split, no compile) — the fast way to
# ask the recompiler whether a set of --s1-cfg regions emits cleanly.
#
# usage: tools/s1_regen.sh OUT.c [DQ8_S1_CFG=..., DQ8_S1_CFG_END=... env]
#
# Mirrors tools/build_split.sh's recompile-arm invocation exactly (same
# extra-entry / noreturn-call lists, same base), so an emission accepted here
# is accepted by the real build.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
OUT=${1:-target/r3-regen.c}
CODE=${DQ8_CODE:-DQ8.code}
[[ -f $CODE ]] || { echo "missing $CODE" >&2; exit 2; }

args=()
while IFS= read -r address; do
  [[ -z "$address" || "$address" == \#* ]] || args+=(--extra-entry "$address")
done < recomp-roots.txt
while IFS= read -r address; do
  [[ -z "$address" || "$address" == \#* ]] || args+=(--noreturn-call "$address")
done < recomp-noreturn.txt

if [[ -n "${DQ8_S1_CFG:-}" ]]; then
  IFS=',' read -r -a s1_entries <<< "$DQ8_S1_CFG"
  IFS=',' read -r -a s1_ends <<< "${DQ8_S1_CFG_END:-}"
  if (( ${#s1_entries[@]} != ${#s1_ends[@]} )); then
    echo "DQ8_S1_CFG=$DQ8_S1_CFG needs ${#s1_entries[@]} DQ8_S1_CFG_END value(s), got ${#s1_ends[@]}" >&2
    exit 2
  fi
  for i in "${!s1_entries[@]}"; do
    args+=(--s1-cfg "${s1_entries[$i]}" --s1-cfg-end "${s1_ends[$i]}")
  done
fi

mkdir -p "$(dirname "$OUT")"
./target/release/dq8tool recompile-arm "$CODE" 0x00100000 "$OUT" "${args[@]}"
ls -l "$OUT"
