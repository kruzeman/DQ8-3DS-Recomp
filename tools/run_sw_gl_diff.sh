#!/usr/bin/env bash
# Matched-frame differential: run the same binary with the software
# rasterizer (DQ8_GL=0) and the GL backend (DQ8_GL=1), dump every guest
# display-transfer frame of both runs (DQ8_DUMP_TRANSFER_PREFIX) and compare
# them frame-by-frame (sha256).  Startup scene (SQUARE ENIX logo + boot) —
# short run, no scenario harness needed.
#
#   tools/run_sw_gl_diff.sh [seconds] [out-dir]
#
# Exit 0 = all matched frames byte-identical (count may differ by a small
# number of fade-boundary frames; those are reported).
set -u
cd "$(dirname "$0")/.."
SECONDS_RUN=${1:-40}
OUT=${2:-target/sw-gl-diff}
CODE=${DQ8_CODE:-/home/hermes/DQ8/DQ8.code}
ROMFS=${DQ8_ROMFS:-/home/hermes/DQ8/target/dq8.romfs}
BIN=${BIN:-target/dq8-tev}

rm -rf "$OUT"
mkdir -p "$OUT/sw" "$OUT/gl" "$OUT/ud-sw" "$OUT/ud-gl"

run_one() {
    local mode=$1 dir=$2 ud=$3
    local gl=0
    [ "$mode" = gl ] && gl=1
    DQ8_GL=$gl DQ8_HEADLESS=1 DQ8_DISPLAY=0 DQ8_RUN_GUEST_THREADS=1 \
      DQ8_AUDIO_SDL=1 SDL_AUDIODRIVER=dummy \
      DQ8_USER_DATA_DIR="$PWD/$ud" DQ8_INPUT_FILE="$PWD/$OUT/none.txt" \
      DQ8_DUMP_TRANSFER_PREFIX="$PWD/$dir/tr" DQ8_PROGRESS_SECONDS=10 \
      "$BIN" "$CODE" "$ROMFS" > "$dir/run.log" 2>&1 &
    local gp=$!
    local t=0
    while kill -0 $gp 2>/dev/null && [ $t -lt $SECONDS_RUN ]; do
        sleep 1
        t=$((t + 1))
    done
    kill -TERM $gp 2>/dev/null
    sleep 2
    kill -KILL $gp 2>/dev/null
    wait $gp 2>/dev/null
    echo "$mode: $(ls "$dir" | grep -c '^tr-') transfer frames in ${t}s"
}

out=$OUT
printf '0\n' > "$OUT/none.txt"
echo "== software run =="
run_one sw "$OUT/sw" "$OUT/ud-sw"
echo "== GL run =="
run_one gl "$OUT/gl" "$OUT/ud-gl"

python3 - "$OUT" <<'EOF'
import hashlib, os, sys
out = sys.argv[1]
def frames(d):
    return sorted(f for f in os.listdir(os.path.join(out, d))
                  if f.startswith("tr-"))
sw, gl = frames("sw"), frames("gl")
n = min(len(sw), len(gl))
bad = 0
for i in range(n):
    a = hashlib.sha256(open(os.path.join(out, "sw", sw[i]), "rb").read()).hexdigest()
    b = hashlib.sha256(open(os.path.join(out, "gl", gl[i]), "rb").read()).hexdigest()
    if a != b:
        bad += 1
        if bad <= 5:
            print("DIFF frame %d: %s vs %s" % (i, sw[i], gl[i]))
print("frames: sw=%d gl=%d matched=%d identical=%d differing=%d" %
      (len(sw), len(gl), n, n - bad, bad))
sys.exit(0 if bad == 0 and n > 100 else 1)
EOF
RC=$?
echo "== grep GL backend line =="
grep -m1 "GL backend: vendor" "$OUT/gl/run.log" || true
grep -m1 "WARNING: software GL" "$OUT/gl/run.log" || true
echo "RESULT: $([ $RC -eq 0 ] && echo PASS || echo FAIL)"
exit $RC
