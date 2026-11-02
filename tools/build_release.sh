#!/usr/bin/env bash
# tools/build_release.sh -- RELEASE build profile for the split runner.
#
#   tools/build_release.sh [output-binary]        (default: target/dq8-rel)
#
# This is a thin wrapper around tools/build_split.sh: the guest dispatch TUs
# are compiled at -O1 instead of the -O0 development default, while the native
# runtime TU (dq8_main.c) keeps its -O2. Nothing else changes -- same generated
# C, same SDL/OpenGL gates, same link line. One flag, no behaviour change.
#
# MEASURED NUMBERS (RX 580, field scene evidence/rx580-holes-20261101/
# scene-holes.sav, 3 runs each, tools/profile.sh):
#
#   mean guest       18.85-19.00 ms   ->  14.89-15.18 ms   (-20%)
#   p90 frame        43.4 ms          ->  37.2 ms
#   frames >33.4 ms  37.0-40.4 %      ->  27.0-30.9 %
#   binary           102 MB           ->  41.6 MB
#   build time       ~2 min           ->  ~2 h
#                    (dq8_part_039 alone takes ~50 min at -O1)
#
# p50 does not improve (21.9 -> 22.5-24.4 ms); the win is in the tail, which is
# the stutter the player actually feels. The ~2 h build is exactly why -O1 is a
# separate profile and not the default: day-to-day iteration stays on -O0.
#
# OBJECT CACHE / PROFILE SWITCHING
# Both profiles compile into the SAME cache directory, $SPLIT/obj (default
# target/dq8-split/obj). The per-TU cache key includes the exact clang command
# line, so switching profile invalidates ALL 36 TUs (35 guest parts + the
# runtime) and costs a FULL rebuild in both directions -- the -O0 objects were
# replaced by the -O1 ones, and vice versa. A profile switch is never cheap.
# Do NOT build two profiles concurrently in one worktree: they share the split
# .c sources and the .o output paths and will clobber each other. Parallel
# profile builds need separate worktrees (or, for a one-off, a separate
# SPLIT_DIR per profile).
#
# Environment (all forwarded unchanged to tools/build_split.sh):
#   BUILD_JOBS=N                  compile pool (default 6; more = more RAM)
#   DQ8_SPLIT_RUNTIME_CFLAGS=...  runtime TU flags (default "-std=c11 -O2 -g0 -w")
#   DQ8_RELEASE_GUEST_CFLAGS=...  guest TU flags; default "-std=c11 -O1 -g0 -w"
#   DQ8_GL=1                      enable the experimental OpenGL backend
#   DQ8_OVERRIDES=...             override registry used by recompile-arm
#   DQ8_SPLIT_SKIP_REGEN=1        reuse the existing split .c files
#   SPLIT_DIR=...                 object/source cache (default target/dq8-split)
# The wrapper always sets DQ8_SPLIT_CFLAGS from DQ8_RELEASE_GUEST_CFLAGS, so a
# stale exported DQ8_SPLIT_CFLAGS from an -O0 shell cannot silently downgrade
# the release profile.
#
# On exit the wrapper prints the exact flag sets tools/build_split.sh used
# (SDL/GL gates included) and the wall time.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

OUT=${1:-target/dq8-rel}
export DQ8_SPLIT_CFLAGS=${DQ8_RELEASE_GUEST_CFLAGS:--std=c11 -O1 -g0 -w}

log=$(mktemp "${TMPDIR:-/tmp}/dq8-release-build.XXXXXX")
trap 'rm -f "$log"' EXIT

echo "== RELEASE profile: guest flags \"$DQ8_SPLIT_CFLAGS\" (dev default is -O0)"
echo "== cache: ${SPLIT_DIR:-target/dq8-split}/obj -- a profile switch recompiles all 36 TUs"
start=$(date +%s)

set +e
bash tools/build_split.sh "$OUT" 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
set -e

elapsed=$(($(date +%s) - start))

# Exact command lines build_split.sh actually used (they include the SDL/GL
# gate flags appended after the profile flags).
grep -E '^== flags: ' "$log" || true
if ((rc == 0)); then
  echo "== RELEASE BUILD OK: $OUT"
else
  echo "== RELEASE BUILD FAILED: rc=$rc (output: $OUT)" >&2
fi
printf '== release profile wall time: %d s (%dh%02dm%02ds)\n' \
  "$elapsed" $((elapsed / 3600)) $((elapsed % 3600 / 60)) $((elapsed % 60))
exit "$rc"
