#!/usr/bin/env bash
# t_5ddc492f: differential test of the generated fragment shaders.
#   tools/run_gl_spec_test.sh            headless EGL (works over SSH)
# Renders every case twice in ONE process (specialised vs generic shader) and
# compares the render-target read-backs byte for byte.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p target
if ! pkg-config --exists egl; then
  echo "missing EGL development files (Debian/Ubuntu: libegl1-mesa-dev)" >&2
  exit 2
fi
cflags=(-std=c11 -O2 -w -DDQ8_ENABLE_GL)
ldflags=()
cflags+=($(pkg-config --cflags egl))
ldflags+=($(pkg-config --libs egl))
clang "${cflags[@]}" tools/gl_spec_test.c -o target/gl_spec_test "${ldflags[@]}" -lm
exec ./target/gl_spec_test "$@"
