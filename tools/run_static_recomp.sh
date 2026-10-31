#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
code_file="${1:-DQ8.code}"
export DQ8_RUN_GUEST_THREADS="${DQ8_RUN_GUEST_THREADS:-1}"
# DQ8_SKIP_MOVIE=1 skips the opening movie (the guest is driven straight to the
# post-intro main-menu state); unset keeps the normal movie playback.  It is a
# runtime flag, so it is passed through the environment as-is, e.g.
#   DQ8_SKIP_MOVIE=1 ./tools/run_static_recomp.sh

if [[ ! -f "$code_file" ]]; then
  echo "Missing decrypted game code: $code_file" >&2
  exit 2
fi
romfs_file="target/dq8.romfs"
if [[ ! -f "$romfs_file" ]]; then
  image="Dragon Quest VIII - Journey of the Cursed King (USA).3ds"
  if [[ ! -f "$image" ]]; then
    echo "Missing extracted RomFS ($romfs_file) and local game image ($image)" >&2
    exit 2
  fi
  ctrtool -q -n 0 "--romfs=$romfs_file" "$image"
fi

args=()
while IFS= read -r address; do
  [[ -z "$address" || "$address" == \#* ]] || args+=(--extra-entry "$address")
done < recomp-roots.txt
while IFS= read -r address; do
  [[ -z "$address" || "$address" == \#* ]] || args+=(--noreturn-call "$address")
done < recomp-noreturn.txt

cargo run --release --quiet -- recompile-arm "$code_file" 0x00100000 target/dq8-startup.c "${args[@]}"
cflags=(-std=c11 -O0 -g0 -w)
ldflags=(-lm)
if [[ "${DQ8_HEADLESS:-0}" != "1" ]] && command -v sdl2-config >/dev/null 2>&1; then
  cflags+=(-DDQ8_ENABLE_SDL)
  # shellcheck disable=SC2207
  cflags+=($(sdl2-config --cflags))
  ldflags+=($(sdl2-config --libs))
elif [[ "${DQ8_HEADLESS:-0}" != "1" ]]; then
  echo "sdl2-config not found; building headless (no display window)" >&2
fi
# Experimental OpenGL rasterization backend (src/pica_gl.c). Compiled in when
# EGL development files are present; enabled at runtime with DQ8_GL=1.
if [[ "${DQ8_GL:-0}" != "0" ]] && [[ -f /usr/include/EGL/egl.h ]] && \
   pkg-config --exists egl 2>/dev/null; then
  cflags+=(-DDQ8_ENABLE_GL)
  # shellcheck disable=SC2207
  cflags+=($(pkg-config --cflags egl))
  ldflags+=($(pkg-config --libs egl))
elif [[ "${DQ8_GL:-0}" == "1" ]]; then
  echo "DQ8_GL=1 requested but EGL development files not found (need pkg-config egl)" >&2
  exit 2
fi
clang "${cflags[@]}" target/dq8-startup.c -o target/dq8-startup "${ldflags[@]}"
exec target/dq8-startup "$code_file" "$romfs_file"
