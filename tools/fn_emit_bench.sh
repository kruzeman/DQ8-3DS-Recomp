#!/usr/bin/env bash
# tools/fn_emit_bench.sh -- in-process A/B for the FN_EMIT pilot (t_8b9fda4c).
#
# Builds target/fn-bench from objects the shipped binary already contains:
#   * tools/fn_emit_bench.c         -- harness (seeds, differential, timing)
#   * $SPLIT/obj/dq8_fnshard_*.o    -- the emitted guest functions (guest -O0)
#   * $SPLIT/obj/dq8_part_*.o       -- the real dispatcher TUs (guest -O0)
#   * dq8_main.o with main() renamed, for the runtime's globals and helpers
# so the dispatch arm is exactly the code the binary runs and the native arm is
# exactly the emitted C, both at the same -O0.
#
# usage: tools/fn_emit_bench.sh [ITERATIONS] [--no-timing]
#        SPLIT_DIR=target/fnpilot-split tools/fn_emit_bench.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

SPLIT=${SPLIT_DIR:-target/dq8-split}
OUT=target/fn-bench
LIST=${DQ8_FN_EMIT_LIST:-tools/fn_emit_top20.txt}
# First non-flag argument is the iteration count; --no-timing is forwarded.
ITERS=20000
FWD=()
for a in "$@"; do
  case $a in
    --no-timing) FWD+=(--no-timing) ;;
    *) ITERS=$a ;;
  esac
done
[[ -f $SPLIT/dq8_rt.h ]] || { echo "missing $SPLIT/dq8_rt.h (build the binary first)" >&2; exit 2; }
ls "$SPLIT"/dq8_fnshard_*.c >/dev/null 2>&1 || {
  echo "missing $SPLIT/dq8_fnshard_*.c (build with DQ8_FN_EMIT_LIST=...)" >&2; exit 2; }
mkdir -p "$OUT"

echo "== function table from $LIST"
python3 tools/fn_emit_bench_gen.py "$LIST" "$OUT" "$SPLIT"

CFLAGS=(-std=c11 -O0 -g0 -w -I"$SPLIT" -I"$OUT")
LDFLAGS=(-lm)
if [[ -z "${DQ8_NO_SDL:-}" ]] && command -v sdl2-config >/dev/null 2>&1; then
  CFLAGS+=(-DDQ8_ENABLE_SDL $(sdl2-config --cflags))
  LDFLAGS+=($(sdl2-config --libs))
  if pkg-config --exists x11 xext 2>/dev/null; then
    CFLAGS+=(-DDQ8_HAVE_X11_PRESENT)
    LDFLAGS+=($(pkg-config --libs x11 xext))
  fi
fi
if [[ -f /usr/include/EGL/egl.h ]] && pkg-config --exists egl 2>/dev/null; then
  CFLAGS+=(-DDQ8_ENABLE_GL $(pkg-config --cflags egl))
  LDFLAGS+=($(pkg-config --libs egl))
fi

echo "== compile harness"
clang "${CFLAGS[@]}" -c tools/fn_emit_bench.c -o "$OUT/harness.o"

echo "== rename dq8_main.o's main() and link against every guest TU + shard"
objcopy --redefine-sym main=dq8_bench_runtime_main "$SPLIT/obj/dq8_main.o" "$OUT/dq8_main_bench.o"
clang "${CFLAGS[@]}" "$OUT/harness.o" "$SPLIT"/obj/dq8_part_*.o \
  "$SPLIT"/obj/dq8_fnshard_*.o "$OUT/dq8_main_bench.o" \
  -o "$OUT/fn-bench" "${LDFLAGS[@]}"

echo "== run"
"$OUT/fn-bench" "$ITERS" "${FWD[@]+"${FWD[@]}"}"
