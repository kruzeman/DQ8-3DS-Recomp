#!/usr/bin/env bash
# recompile_try.sh ROOTS_FILE [OUT_C] — run ONLY the recompile-arm stage with
# the given roots file (plus the shared recomp-noreturn.txt), skipping
# split/compile/link.  Fails fast on a bad root entry (runaway translation).
# usage: tools/recompile_try.sh /tmp/lit_missing.txt /tmp/try.c
set -u
cd /home/hermes/DQ8 || exit 1
ROOTS=$1
OUT=${2:-/tmp/try-dq8.c}
args=()
while IFS= read -r a; do
  [[ -z "$a" || "$a" == \#* ]] || args+=(--extra-entry "$a")
done < "$ROOTS"
while IFS= read -r a; do
  [[ -z "$a" || "$a" == \#* ]] || args+=(--noreturn-call "$a")
done < recomp-noreturn.txt
cargo run --release --quiet -- recompile-arm DQ8.code 0x00100000 "$OUT" "${args[@]}"
rc=$?
echo "recompile exit=$rc roots=$((${#args[@]}))"
exit $rc
