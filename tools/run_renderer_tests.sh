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
echo "== build test_gl_depth_order (per-fragment depth ordering) =="
cc "${cflags[@]}" -D_DEFAULT_SOURCE tools/test_gl_depth_order.c \
   -o target/test_gl_depth_order "${ldflags[@]}" -lm
echo "== run test_gl_depth_order (live imageLoad, then per-draw copy) =="
./target/test_gl_depth_order
DQ8_GL_FBFETCH=0 ./target/test_gl_depth_order
echo "== run test_gl_depth_order (IN-SHADER fallback path, DQ8_GL_DEPTH_ATTACH=0) =="
DQ8_GL_DEPTH_ATTACH=0 ./target/test_gl_depth_order
echo "== reference only: IN-SHADER + per-draw copies reproduces the pre-fix"
echo "   defect (expects 'RESULT: N FAILED' with model case 143/143) =="
DQ8_GL_DEPTH_ATTACH=0 DQ8_GL_FBFETCH=0 ./target/test_gl_depth_order || true
echo "== build + run gl_backend_test =="
exec tools/run_gl_test.sh "$@"
