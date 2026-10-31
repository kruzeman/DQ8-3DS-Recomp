#!/usr/bin/env bash
# Auto-rebuild loop: run guest, collect escaped PCs, regenerate, repeat.
# Usage: tools/escape_loop.sh [max_iter] [run_seconds]
set -uo pipefail
cd "$(dirname "$0")/.."

MAX_ITER="${1:-30}"
RUN_SEC="${2:-60}"
BIN=target/dq8-startup
LOG=/tmp/dq8-escape-loop.log
ERR=/tmp/dq8-escape-loop.err

gen_and_build() {
  rm -f target/dq8-startup.c
  cargo run --release --quiet -- recompile-arm DQ8.code 0x00100000 target/dq8-startup.c \
    $(awk 'NF && $1!~/^#/{printf "--extra-entry %s ", $1}' recomp-roots.txt) \
    $(awk 'NF && $1!~/^#/{printf "--noreturn-call %s ", $1}' recomp-noreturn.txt) \
    > /tmp/dq8-gen.log 2>&1 || return 1
  clang -std=c11 -O0 -g0 -w target/dq8-startup.c -o "$BIN" -lm > /tmp/dq8-cc.log 2>&1 || return 1
  return 0
}

echo "=== initial build ==="
gen_and_build || { echo "BUILD FAILED"; tail -5 /tmp/dq8-gen.log /tmp/dq8-cc.log; exit 1; }

for i in $(seq 1 "$MAX_ITER"); do
  echo "=== iter $i: run ${RUN_SEC}s ==="
  rm -f "$ERR"
  timeout "$RUN_SEC" env DQ8_RUN_GUEST_THREADS=1 DQ8_HEADLESS=1 \
    DQ8_TRACE_GPU=1 DQ8_PROGRESS_SECONDS=20 \
    "$BIN" DQ8.code target/dq8.romfs > "$LOG" 2> "$ERR"
  rc=$?
  escapes=$(grep -oP 'translated PC escaped to \K[0-9A-Fa-f]+' "$ERR" | sort -u)
  n_xfer=$(grep -c 'GSP display-transfer' "$ERR")
  n_draw=$(grep -c 'draw trigger' "$ERR")
  n_xfer=${n_xfer:-0}; n_draw=${n_draw:-0}
  echo "  exit=$rc xfers=$n_xfer draws=$n_draw"
  grep 'unimplemented' "$ERR" | tail -3
  if [ -z "$escapes" ]; then
    echo "  no new escapes — done or stuck (exit=$rc)"
    grep -E 'progress:|snapshot' "$ERR" | tail -3
    break
  fi
  added=0
  for pc in $escapes; do
    v=$((16#$pc))
    if [ "$v" -lt $((0x100000)) ] || [ "$v" -ge $((0x5FA000)) ]; then
      echo "  ! skip bogus PC 0x$pc"
      continue
    fi
    addr="0x$pc"
    if ! grep -qi "^${addr}$" recomp-roots.txt; then
      echo "$addr" >> recomp-roots.txt
      echo "  + root $addr"
      added=$((added+1))
    fi
  done
  if [ "$added" -eq 0 ]; then
    echo "  all escapes already in roots — stuck"
    break
  fi
  echo "  rebuilding with $added new roots..."
  gen_and_build || { echo "BUILD FAILED"; tail -5 /tmp/dq8-gen.log; exit 1; }
done
echo "=== loop done ==="
