#!/usr/bin/env bash
# Renderer regression tests: software/OpenGL pixel differential (A14/A15 +
# full-TEV-chain randomized differential) and the GL backend smoke test.
#
#   tools/run_renderer_tests.sh          headless (EGL surfaceless, works over SSH)
#   tools/run_renderer_tests.sh --window tools/run_gl_test.sh --window mode
#
# Exit code 0 = all checks passed.
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

echo "== build test_pica_gl_diff (SW/GL pixel differential) =="
cc "${cflags[@]}" tools/test_pica_gl_diff.c -o target/test_pica_gl_diff "${ldflags[@]}" -lm
echo "== run test_pica_gl_diff =="
./target/test_pica_gl_diff
echo "== build + run gl_backend_test =="
exec tools/run_gl_test.sh "$@"
