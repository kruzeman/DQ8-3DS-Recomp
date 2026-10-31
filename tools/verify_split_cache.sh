#!/usr/bin/env bash
# Verify whether a split object cache's per-TU stamps still match the current
# sources/flags (same key construction as tools/build_split.sh compile_cached).
# Usage: SPLIT_DIR=<dir> [DFLAGS="..."] [RFLAGS="..."] bash tools/verify_split_cache.sh
# Prints OK/STALE per TU; exit 0 iff everything would be reused.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.."
SPLIT=${SPLIT_DIR:-target/dq8-split}
DFLAGS=${DQ8_SPLIT_CFLAGS:--std=c11 -O2 -g0 -w}
RFLAGS=${DQ8_SPLIT_RUNTIME_CFLAGS:--std=c11 -O2 -g0 -w}

cflags=($DFLAGS)
rcflags=($RFLAGS)
ldflags=(-lm)
if [[ -z "${DQ8_NO_SDL:-}" ]] && command -v sdl2-config >/dev/null 2>&1; then
  gate=(-DDQ8_ENABLE_SDL $(sdl2-config --cflags))
  cflags+=("${gate[@]}")
  rcflags+=("${gate[@]}")
  if pkg-config --exists x11 xext 2>/dev/null; then
    rcflags+=(-DDQ8_HAVE_X11_PRESENT)
  fi
fi
if [[ "${DQ8_GL:-0}" != "0" ]] && [[ -f /usr/include/EGL/egl.h ]] && pkg-config --exists egl 2>/dev/null; then
  cflags+=(-DDQ8_ENABLE_GL)
  rcflags+=(-DDQ8_ENABLE_GL)
  gate=($(pkg-config --cflags egl))
  cflags+=("${gate[@]}")
  rcflags+=("${gate[@]}")
fi

CLANG_ID=$(clang --version)
HDR_HASH=$(sha256sum "$SPLIT/dq8_rt.h" | cut -d' ' -f1)
OBJDIR="$SPLIT/obj"
ok=0; stale=0
check() {
  local src=$1 obj=$2
  local -n flagset=$3
  local -a cmd=(clang "${flagset[@]}" -I"$SPLIT" -c "$src" -o "$obj")
  local key
  key=$(printf 'cmd=%s\nclang=%s\nhdr=%s\nsrc=%s\n' \
        "$(printf '%q ' "${cmd[@]}")" "$CLANG_ID" "$HDR_HASH" \
        "$(sha256sum "$src" | cut -d' ' -f1)" | sha256sum | cut -d' ' -f1)
  if [[ -f "$obj" && -f "$obj.stamp" && "$(<"$obj.stamp")" == "$key" ]]; then
    echo "OK    $(basename "$obj")"; ok=$((ok+1))
  else
    echo "STALE $(basename "$obj")"; stale=$((stale+1))
  fi
}
check "$SPLIT/dq8_main.c" "$OBJDIR/dq8_main.o" rcflags
for part in "$SPLIT"/dq8_part_*.c; do
  check "$part" "$OBJDIR/$(basename "$part" .c).o" cflags
done
echo "== cache: $ok reusable, $stale stale =="
exit $(( stale > 0 ))
