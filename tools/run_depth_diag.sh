#!/bin/bash
# t_599a43f7 — DEPTH PATH DIAGNOSTIC (run this on the affected machine).
#
#   tools/run_depth_diag.sh [savestate] [steps]
#
# Runs the SAME build three times from the same savestate, changing only the two
# knobs that select the depth path, and prints the numbers plus the log lines
# that identify the failure:
#
#   A  your normal configuration            (DQ8_SPLIT_GL default, D32F depth ON)
#   B  DQ8_SPLIT_GL=0, D32F depth ON        (synchronous GL path)
#   C  DQ8_SPLIT_GL=1, DQ8_GL_DEPTH_ATTACH=0 (worker thread, in-shader depth)
#
# Interpretation (measured on the reference machine, llvmpipe):
#   A dark, B and C normal  -> the DQ8_SPLIT_GL worker + hardware depth path
#                              (this was the operator's black screen, fixed)
#   A normal, B, C normal   -> no problem in this scene: the bug is elsewhere
#   A and B dark, C normal  -> the hardware depth path itself on this driver
#   everything dark         -> the scene/fade is black, use a different savestate
#
# Output goes to $PWD/depth-diag-<timestamp>/ ; paste the SUMMARY block (the
# whole run.log of pass A is useful too, it is small).
set -u
SAV=${1:-}
STEPS=${2:-20000000}
BIN=${BIN:-target/dq8-final}
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$PWD/depth-diag-$(date +%Y%m%d-%H%M%S)
if [ -z "$SAV" ] || [ ! -f "$SAV" ]; then
    for cand in "$HERE/field.sav" "$HERE/saves/dq8-state.sav" "$HERE/scene-holes.sav"; do
        [ -f "$cand" ] && SAV=$cand && break
    done
fi
if [ -z "$SAV" ] || [ ! -f "$SAV" ]; then
    echo "usage: $0 <savestate.sav> [steps]" >&2
    echo "  (no savestate given and none of the usual ones found)" >&2
    exit 2
fi
mkdir -p "$OUT"
echo "binary : $BIN"
echo "state  : $SAV"
echo "steps  : $STEPS"
echo "out    : $OUT"
echo

pass() { # <tag> <extra env...>
    local tag=$1; shift
    local dir="$OUT/$tag"
    mkdir -p "$dir/cap"
    echo "--- pass $tag ($*)"
    ( cd "$HERE" && env DQ8_HEADLESS=1 DQ8_GL=1 DQ8_CAPTURE_PPM=1 \
        DQ8_CAPTURE_PPM_DIR="$dir/cap" DQ8_BENCH_STEPS="$STEPS" "$@" \
        timeout --signal=TERM --kill-after=5 900 "$BIN" --load-state "$SAV" \
        DQ8.code target/dq8.romfs > "$dir/run.log" 2>&1 )
    echo "    exit=$? log=$dir/run.log"
    grep -E "DEPTH DIAG|depth path =|self-test|GL ERROR|DEPTH PATH FALLBACK|GL DEPTH GUARD|plausible scene|WARNING: dark" \
        "$dir/run.log" | head -20 | sed 's/^/    /'
    if command -v python3 >/dev/null 2>&1 && [ -f "$dir/cap/dq8-screen.ppm" ]; then
        python3 - "$dir/cap/dq8-screen.ppm" <<'PY' | sed 's/^/    /'
import sys
d = open(sys.argv[1], 'rb').read()
i, vals = 2, []
while len(vals) < 3:
    while d[i:i+1].isspace():
        i += 1
    s = i
    while not d[i:i+1].isspace():
        i += 1
    vals.append(int(d[s:i]))
i += 1
w, h, mx = vals
px = d[i:i + w * h * 3]
n, dark, lum = w * h, 0, 0
for k in range(n):
    r, g, b = px[k*3], px[k*3+1], px[k*3+2]
    if r <= 16 and g <= 16 and b <= 16:
        dark += 1
    lum += (r*299 + g*587 + b*114)//1000
print('frame: dark=%.1f%% mean_luma=%.1f (%dx%d)' % (100.0*dark/n, lum/n, w, h))
PY
    fi
    echo
}

pass A
pass B DQ8_SPLIT_GL=0 DQ8_GL_DEPTH_ATTACH=1
pass C DQ8_SPLIT_GL=1 DQ8_GL_DEPTH_ATTACH=0

echo "=========================== SUMMARY ==========================="
for tag in A B C; do
    f="$OUT/$tag/cap/dq8-screen.ppm"
    line=$(grep -h "DEPTH DIAG" "$OUT/$tag/run.log" | head -1)
    printf '%-2s %s\n' "$tag" "$line"
done
echo "Paste this block plus the log of the pass that looks wrong."
echo "Full logs: $OUT/{A,B,C}/run.log"
