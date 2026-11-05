#!/usr/bin/env bash
# tools/fn_emit_evidence.sh -- evidence run for the FN_EMIT spike (t_f110c9ca).
#
#  1. deterministic 2M-step field run with DQ8_TRACE_GPU=1, gate OFF vs ON:
#     the GPU/PICA stream must be byte-identical (the session's authoritative
#     "the region path is exact" check);
#  2. field A/B: interleaved gate-off/on pairs with DQ8_FRAMEPROF=1, mean guest
#     ms over frames that actually rendered.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
BIN=${BIN:-target/dq8-fnemit}
STATE=${STATE:-/home/hermes/DQ8/evidence/rx580-holes-20261101/scene-holes.sav}
OUT=${OUT:-target/fn-emit-evidence}
mkdir -p "$OUT"

BASE_ENV=(DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_VSPAR=4 DQ8_SPLIT_GL=0
          DQ8_STACK_NO_RELOCATE=1 DQ8_GL_DEPTH_ATTACH=1)

run() { # run <log> <gate: off|on> <extra env...>
  local log=$1 gate=$2; shift 2
  local -a envs=("${BASE_ENV[@]}" "$@")
  if [[ $gate == on ]]; then envs+=(DQ8_FN_EMIT=0x0016A058); else envs+=(DQ8_FN_EMIT=); fi
  env "${envs[@]}" timeout -k 10 "${RUN_TIMEOUT:-120}" "$BIN" --load-state "$STATE" \
      DQ8.code target/dq8.romfs > "$log.out" 2> "$log.err"
  echo "  $log rc=$? frames=$(grep -c 'FRAMEPROF f=' "$log.err" || true)"
}

echo "== 1. deterministic 2M-step PICA trace, gate off vs on"
for arm in off on; do
  run "$OUT/trace-$arm" "$arm" DQ8_TRACE_GPU=1 DQ8_BENCH_STEPS=2000000
done
# Everything the guest asks the GPU/display to do, plus the bench line; the
# S1 checks use the same stream.
for arm in off on; do
  grep -E 'GPU|PICA|draw|DRAW|XFER|BENCH|TEX|SWAP' "$OUT/trace-$arm.err" > "$OUT/trace-$arm.gpu"
  echo "  $arm: $(wc -l < "$OUT/trace-$arm.gpu") trace lines  sha256=$(sha256sum < "$OUT/trace-$arm.gpu" | cut -c1-16)"
done
if cmp -s "$OUT/trace-off.gpu" "$OUT/trace-on.gpu"; then
  echo "  PICA TRACE: BYTE-IDENTICAL"
else
  echo "  PICA TRACE: DIFFERENT"
  diff "$OUT/trace-off.gpu" "$OUT/trace-on.gpu" | head -5
fi
for arm in off on; do grep -E 'FN_EMIT|BENCH ' "$OUT/trace-$arm.err" | tail -2; done

echo "== 2. field A/B (interleaved pairs, 12 s each, FRAMEPROF guest_ms)"
SUM=0
for pair in 1 2 3; do
  for arm in off on; do
    RUN_TIMEOUT=25 run "$OUT/ab-$pair-$arm" "$arm" DQ8_FRAMEPROF=1 >/dev/null
  done
  python3 - "$OUT" "$pair" <<'PY'
import re, sys
out, pair = sys.argv[1], sys.argv[2]
def mean_guest(path):
    vals = []
    for line in open(path, errors='ignore'):
        m = re.match(r'FRAMEPROF f=\d+ t=[\d.]+ total_ms=([\d.]+) guest_ms=([\d.]+)', line)
        if m and float(m.group(1)) > 2.0:
            vals.append(float(m.group(2)))
    vals.sort()
    return (sum(vals)/len(vals) if vals else float('nan'), len(vals))
o, no = mean_guest(f"{out}/ab-{pair}-off.err")
n, nn = mean_guest(f"{out}/ab-{pair}-on.err")
print(f"  pair {pair}: off guest_ms={o:.3f} (n={no})  on guest_ms={n:.3f} (n={nn})  delta={o-n:+.3f}")
PY
done
grep -hE 'FN_EMIT entry' "$OUT"/ab-*.err | tail -3
