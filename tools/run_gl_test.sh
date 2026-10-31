#!/usr/bin/env bash
# Reproducible OpenGL-backend test (no game data required).
#
#   tools/run_gl_test.sh                 headless EGL test (works over SSH)
#   tools/run_gl_test.sh --window 30     SDL2 window, visible on a desktop PC
#
# The test prints GL_VENDOR/GL_RENDERER/GL_VERSION. On a machine with a GPU
# the renderer line names the GPU (NVIDIA/AMD/Intel) and the draws run in
# hardware; on a server without a GPU Mesa reports llvmpipe (software).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p target

if ! pkg-config --exists egl; then
  echo "missing EGL development files (Debian/Ubuntu: libegl1-mesa-dev)" >&2
  exit 2
fi

cflags=(-std=c11 -O2 -Wall -DDQ8_ENABLE_GL)
ldflags=()
# shellcheck disable=SC2207
cflags+=($(pkg-config --cflags egl))
ldflags+=($(pkg-config --libs egl))
if command -v sdl2-config >/dev/null 2>&1; then
  # shellcheck disable=SC2207
  cflags+=(-DGL_TEST_ENABLE_SDL $(sdl2-config --cflags))
  ldflags+=($(sdl2-config --libs))
else
  echo "sdl2-config not found; building without --window support" >&2
fi

cc "${cflags[@]}" tools/gl_backend_test.c -o target/gl_backend_test "${ldflags[@]}" -lm
exec target/gl_backend_test "$@"
