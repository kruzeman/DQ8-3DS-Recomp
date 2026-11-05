#!/usr/bin/env bash
# vspar_det.sh -- deterministic identity check for the vertex-pool card.
#
#   tools/vspar_det.sh <binary> <arm> <vspar> [steps] [scenes...]
#
# One deterministically stepped run per scene (DQ8_DETERMINISTIC=1,
# DQ8_BENCH_STEPS=8M by default) with the full GPU/PICA command trace enabled.
# Stores, per scene: the PICA|GSP|DRAW|SHADER trace lines (count + sha256), the
# DET_RAM / DET_FRAME / DET_CLOCK digests, and a sha256 over the concatenated
# DQ8_DUMP_PICA_ALL command lists.  Two builds/arms with the same numbers are
# bit-identical through the whole draw path, not just the final frame.
set -u
BIN=$(realpath "${1:?binary}"); ARM=${2:?arm}; VSPAR=${3:?vspar}
STEPS=${4:-8000000}; shift 4 2>/dev/null || shift $#
SCENES=${*:-city battle field}
HOT=${HOT:-$HOME/hotreg}
OUT=${DET_OUT:-$HOT/vs-det}
mkdir -p "$OUT"
cd "$HOME/DQ8" || exit 2

for sc in $SCENES; do
  st=$HOT/states/$sc.sav
  tag=$sc-$ARM
  dir=$OUT/pica-$tag
  mkdir -p "$dir"
  env DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_SPLIT_GL=0 \
      DQ8_FN_EMIT=all DQ8_DETERMINISTIC=1 DQ8_BENCH_STEPS="$STEPS" \
      DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$dir" DQ8_TRACE_GPU=1 \
      DQ8_DUMP_PICA_ALL="$dir/pica" DQ8_VSPAR="$VSPAR" \
      DQ8_USER_DATA_DIR="$HOT/ud-vs-det" \
    timeout -s KILL 900 "$BIN" "$HOME/DQ8/DQ8.code" "$HOME/DQ8/dq8.romfs" \
      --load-state "$st" >/dev/null 2>"$OUT/$tag.log"
  rc=$?
  # Normalise the per-arm dump directory out of the capture lines: the only
  # non-deterministic token in the trace is that path, and it must not be
  # allowed to hide (or fake) a real difference.
  grep -E '^(PICA|GSP|DRAW|SHADER)' "$OUT/$tag.log" | sed "s#$dir/##g" \
    >"$OUT/$tag.trace"
  grep -E '^DET_(RAM|FRAME|CLOCK)' "$OUT/$tag.log" >"$OUT/$tag.det"
  n=$(ls "$dir"/*.bin 2>/dev/null | wc -l)
  (cd "$dir" && cat $(ls *.bin 2>/dev/null | sort) 2>/dev/null | sha256sum | cut -d' ' -f1) \
    >"$OUT/$tag.pica"
  printf '%-12s rc=%-3s trace_lines=%-7s trace_sha=%s pica_lists=%-4s pica_sha=%s\n' \
    "$tag" "$rc" "$(wc -l <"$OUT/$tag.trace")" \
    "$(sha256sum <"$OUT/$tag.trace" | cut -c1-16)" "$n" \
    "$(cut -c1-16 "$OUT/$tag.pica")"
  grep -E '^DET_(FRAME|CLOCK)' "$OUT/$tag.log" | sed 's/ path=.*//' | sed 's/^/             /'
  grep -E '^DET_RAM' "$OUT/$tag.log" | sed 's/^/             /'
done
