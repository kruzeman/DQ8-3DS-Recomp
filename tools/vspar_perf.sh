#!/usr/bin/env bash
# vspar_perf.sh -- per-thread / per-symbol `perf` table for the vertex pool.
#
#   tools/vspar_perf.sh <binary> <tag> [secs] [scene] [VAR=val...]
#
# perf record slows the game down (measured 9.8 fps while profiling), so this
# is for SHARES and per-thread attribution only -- fps numbers come from
# vspar_ab.sh.  Rootless perf from the operator's tree.
set -u
BIN=$(realpath "${1:?binary}"); TAG=${2:?tag}; SECS=${3:-12}; SCENE=${4:-city}
shift 4 2>/dev/null || shift $#
HOT=${HOT:-$HOME/hotreg}
D=$HOT/vs-perf/$TAG
mkdir -p "$D" "$HOT/ud-vs-perf-$TAG"
cd "$HOME/DQ8" || exit 2
export LD_LIBRARY_PATH=$HOME/perfroot/usr/lib:$HOME/perfroot/usr/lib64
PERF=$HOME/perfroot/usr/bin/perf

env DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_SPLIT_GL=0 \
    DQ8_FN_EMIT=all DQ8_VSPAR=8 DQ8_USER_DATA_DIR="$HOT/ud-vs-perf-$TAG" "$@" \
  "$PERF" record -e cycles:u -F 999 -o "$D/perf.data" -- \
  timeout -s KILL "$((SECS + 4))" "$BIN" --load-state "$HOT/states/$SCENE.sav" \
    DQ8.code dq8.romfs >"$D/out.txt" 2>"$D/err.txt"
rc=$?
"$PERF" report -i "$D/perf.data" --stdio --sort symbol 2>/dev/null | head -18 >"$D/syms.txt"
"$PERF" report -i "$D/perf.data" --stdio --sort tid 2>/dev/null | head -24 >"$D/tids.txt"
"$PERF" annotate -i "$D/perf.data" --stdio -s pvs_par_worker 2>/dev/null \
  | grep -E '^ +[0-9]+\.[0-9]+' | head -12 >"$D/annot.txt"
echo "== $TAG rc=$rc =="
sed -n '1,12p' "$D/syms.txt"
echo "-- per tid (top) --"
sed -n '1,14p' "$D/tids.txt"
echo "-- pvs_par_worker hot instructions --"
cat "$D/annot.txt"
