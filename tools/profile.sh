#!/usr/bin/env bash
# profile.sh — one-shot performance profile of a DQ8 run.
#
#   ./profile.sh <savestate.sav> [seconds]      (default 30 s)
#
# Produces:
#   prof-<timestamp>/frameprof.log   frame phase breakdown (FRAMEPROF)
#   prof-<timestamp>/glphase.log     per-frame GL/PICA counters (GLPHASE)
#   prof-<timestamp>/tempo.log       guest clock vs wall clock (TEMPO)
#   prof-<timestamp>/perf.data       CPU profile, if `perf` is available
#   prof-<timestamp>/SUMMARY.txt     the numbers that matter
#
# Everything stays on disk; only SUMMARY.txt is meant to be sent back.
set -u
STATE=${1:?usage: profile.sh <savestate.sav> [seconds]}
SECS=${2:-30}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/prof-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"

BIN="$HERE/dq8"
[ -x "$BIN" ] || { echo "dq8 binary not found next to this script"; exit 2; }
CODE=${DQ8_CODE:-$HERE/game/DQ8.code}
ROMFS=${DQ8_ROMFS:-$HERE/game/dq8.romfs}
[ -e "$CODE" ] || CODE=$(ls "$HERE"/game/*.code 2>/dev/null | head -1)
[ -e "$ROMFS" ] || ROMFS=$(ls "$HERE"/game/*.romfs 2>/dev/null | head -1)

export DQ8_GL=1 DQ8_GL_TEXCACHE=1 DQ8_RUN_GUEST_THREADS=1 DQ8_S1_CFG=all
export DQ8_SPLIT_GL=${DQ8_SPLIT_GL:-0} DQ8_GL_DEPTH_ATTACH=${DQ8_GL_DEPTH_ATTACH:-1}
export DQ8_VSPAR=${DQ8_VSPAR:-4} DQ8_STACK_NO_RELOCATE=1
export DQ8_FRAMEPROF=1 DQ8_TEMPO=1 DQ8_GL_DEBUG=1

echo "state=$STATE  seconds=$SECS  binary=$BIN"
echo "output=$OUT"

# Optional CPU profile of the whole process (sampling every 1 ms).
PERF=""
command -v perf >/dev/null && PERF="perf"
run_game() {
  timeout -k 5 "$SECS" "$BIN" --load-state "$STATE" "$CODE" "$ROMFS" \
    > "$OUT/frameprof.log" 2> "$OUT/glphase.log"
}
if [ -n "$PERF" ]; then
  # perf may be blocked by perf_event_paranoid; fall back cleanly if it fails.
  perf record -F 999 -g -o "$OUT/perf.data" -- \
    timeout -k 5 "$SECS" "$BIN" --load-state "$STATE" "$CODE" "$ROMFS" \
    > "$OUT/frameprof.log" 2> "$OUT/glphase.log" || true
  if ! grep -q 'FRAMEPROF' "$OUT/frameprof.log" 2>/dev/null; then
    echo "perf unavailable (perf_event_paranoid?) - re-running without it"
    PERF=""
    run_game
  fi
else
  run_game
fi

# TEMPO shares the same stderr stream as GLPHASE; split it out.
grep '^TEMPO ' "$OUT/glphase.log" > "$OUT/tempo.log" 2>/dev/null

{
  echo "== frames"
  python3 - "$OUT" <<'PY'
import sys, re, glob, os
out = sys.argv[1]
lines = []
for p in (os.path.join(out,'frameprof.log'), os.path.join(out,'glphase.log')):
    try: lines += open(p, errors='ignore').read().splitlines()
    except OSError: pass
frames = []
for l in lines:
    m = re.match(r'FRAMEPROF f=(\d+) t=([\d.]+) total_ms=([\d.]+) guest_ms=([\d.]+) gl_ms=([\d.]+) present_ms=([\d.]+)', l)
    if m: frames.append(tuple(float(x) for x in m.groups()[1:]))
if frames:
    heavy = [f for f in frames if f[1] > 5.0]   # frames that actually rendered
    src = heavy if heavy else frames
    tot = [f[1] for f in src]; tot.sort()
    n = len(tot)
    print(f"  scan-outs={len(frames)}  rendered={len(heavy)}")
    print(f"  RENDER frames: p50={tot[n//2]:.1f} ms  p90={tot[int(n*0.9)]:.1f} ms  max={tot[-1]:.1f} ms")
    slow = sum(1 for t in tot if t > 33.4)
    print(f"  rendered frames slower than 33.4 ms: {slow} ({100.0*slow/n:.1f}%)")
    g = sum(f[2] for f in src)/n; gl = sum(f[3] for f in src)/n; pr = sum(f[4] for f in src)/n
    print(f"  mean guest={g:.2f} ms  gl={gl:.2f} ms  present={pr:.2f} ms")
else:
    print("  no FRAMEPROF lines (run with DQ8_FRAMEPROF=1)")
sums = [l for l in lines if 'FRAMEPROF-SUM' in l]
if sums: print('  ' + sums[-1].split('FRAMEPROF-SUM')[-1].strip()[:200])
PY
  echo
  echo "== guest clock (TEMPO)"
  grep '^TEMPO ' "$OUT/glphase.log" 2>/dev/null | tail -1 | cut -c1-260
  echo
  echo "== per-second GL/PICA"
  grep 'GLPHASE' "$OUT/glphase.log" 2>/dev/null | tail -1 | cut -c1-260
} > "$OUT/SUMMARY.txt" 2>&1

echo
echo "---- SUMMARY (send this back) ----"
cat "$OUT/SUMMARY.txt"
