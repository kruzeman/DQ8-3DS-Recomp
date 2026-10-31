#!/usr/bin/env bash
# Regenerate the translated C from the current recompiler + runtime, split it
# into many translation units and build the runner.
#
# usage: tools/build_split.sh [output-binary]
#
# Runs from the repository root that owns this script (worktree-safe). SDL2 and
# the experimental OpenGL backend are compiled in under the same gates as
# tools/run_static_recomp.sh (DQ8_HEADLESS=1 disables SDL; DQ8_GL!=0 enables
# the EGL/OpenGL backend), so one binary serves both the headless checks and
# the desktop test on a GPU machine.
#
# The generated interpreter is a single 168 MB C file whose dispatch switch
# clang cannot compile inside this worker's 4 GiB cgroup (see
# DQ8_OPENING_CINEMATIC_PROGRESS.md §2), so tools/split_dispatch.py is used to
# cut it into one TU per 32-page ADDRESS band (see split_dispatch.py), which
# are compiled in a bounded pool.
#
# Unchanged TUs are NOT recompiled.  Before compiling dq8_part_NNN.c the script
# hashes (a) the .c file contents, (b) the dq8_rt.h contents it includes,
# (c) the exact clang command line and (d) `clang --version`, and reuses the
# existing obj/dq8_part_NNN.o when that hash still matches the sidecar stamp
# obj/dq8_part_NNN.o.stamp written at compile time.  A changed source, header,
# flag set or compiler therefore still triggers a correct rebuild, while a
# no-change rebuild skips every per-TU clang invocation and only relinks.
#
# The 37 guest dispatch TUs and the native runtime (dq8_main.c: the PICA
# software rasteriser, guest memory helpers, capture/present, service shims)
# are compiled with SEPARATE flag sets (audit PERF-03).  The guest parts keep
# the O0 default they have always had, so their objects stay bit-identical;
# the runtime TU defaults to -O2, which the audit measured at 3.48x on the
# 100M-main-instruction modal budget (51.733 s -> 14.865 s).
#
# Knobs (all optional; DQ8_SPLIT_CFLAGS default reproduces the guest part of
# the previous behaviour exactly):
#   BUILD_JOBS=N            parallel clang pool size (default 6)
#   DQ8_SPLIT_CFLAGS="..."  GUEST dispatch-TU flags (default "-std=c11 -O0 -g0 -w");
#                           any flag change invalidates the cached guest .o
#   DQ8_SPLIT_RUNTIME_CFLAGS="..."
#                           NATIVE runtime (dq8_main.c) flags
#                           (default "-std=c11 -O2 -g0 -w"); set it to
#                           "-std=c11 -O0 -g0 -w" to reproduce the old binary
#   DQ8_SPLIT_SKIP_REGEN=1  skip recompile-arm + split_dispatch and rebuild
#                           only the C (compile + link) from the existing
#                           $SPLIT/*.c — handy to iterate on compile flags
#   DQ8_S1_BLOCK=0xADDR     S1b: also emit the Block-IR straight-line block at
#                           ADDR into the generated C (recompile-arm
#                           --s1-block).  Unset: generated C is byte-identical
#                           to a build without this knob.  The same env name
#                           gates the hook at runtime.
#   DQ8_S1_CFG=0xADDR + DQ8_S1_CFG_END=0xADDR
#                           S1c: emit the whole region [ADDR, ADDR_END) as one
#                           native CFG (direct intra-region edges, exits fall
#                           back to the dispatcher) via recompile-arm --s1-cfg
#                           --s1-cfg-end.  Unset: generated C is byte-identical
#                           to a build without this knob.  The same env name
#                           DQ8_S1_CFG=<entry> gates the hook at runtime.
#
# Both flag sets are part of the per-TU cache key (the exact clang command line
# is hashed), so flipping either set rebuilds only the TUs it affects: going
# O0 -> O2 on the runtime recompiles dq8_main.o alone and reuses all 37 guest
# parts.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

OUT=${1:-target/dq8-startup-split}
SPLIT=${SPLIT_DIR:-target/dq8-split}
JOBS=${BUILD_JOBS:-6}
# Guest dispatch TUs (37 parts) vs native runtime TU (dq8_main.c).  Two
# independent flag sets, both hashed into the per-TU cache key below.
DFLAGS=${DQ8_SPLIT_CFLAGS:--std=c11 -O0 -g0 -w}
RFLAGS=${DQ8_SPLIT_RUNTIME_CFLAGS:--std=c11 -O2 -g0 -w}

# SDL2 gate (restored after the 91755e0 merge dropped it; same gate as
# worktrees/audio/tools/build_split.sh): DQ8_HEADLESS=1 keeps runs headless,
# but the binary must still be able to open a window on a desktop PC.
# src/pica_gl.c / -DDQ8_ENABLE_GL live on the renderer (Stage 2) branch only,
# so the GL gate is intentionally NOT enabled here.
cflags=($DFLAGS)    # guest dispatch parts  (dq8_part_NNN.c)
rcflags=($RFLAGS)   # native runtime TU      (dq8_main.c)
CODE=${DQ8_CODE:-DQ8.code}
if [[ ! -f $CODE ]]; then echo "missing $CODE" >&2; exit 2; fi
ldflags=(-lm)
if [[ -z "${DQ8_NO_SDL:-}" ]] && command -v sdl2-config >/dev/null 2>&1; then
  gate=(-DDQ8_ENABLE_SDL $(sdl2-config --cflags))
  cflags+=("${gate[@]}")
  rcflags+=("${gate[@]}")
  ldflags+=($(sdl2-config --libs))
  # F2: X11 SHM present path for the non-GL window surface (runtime-only, see
  # host_x11_present_* in the runtime prelude).  libx11-dev/libxext-dev are
  # hard dependencies of libsdl2-dev, so the headers are present whenever SDL
  # is; without them the runtime keeps the plain SDL surface path.
  if pkg-config --exists x11 xext 2>/dev/null; then
    rcflags+=(-DDQ8_HAVE_X11_PRESENT)
    ldflags+=($(pkg-config --libs x11 xext))
  fi
fi

args=()
while IFS= read -r address; do
  [[ -z "$address" || "$address" == \#* ]] || args+=(--extra-entry "$address")
done < recomp-roots.txt
while IFS= read -r address; do
  [[ -z "$address" || "$address" == \#* ]] || args+=(--noreturn-call "$address")
done < recomp-noreturn.txt

if [[ -z "${DQ8_SPLIT_SKIP_REGEN:-}" ]]; then
  echo "== recompile-arm: ${#args[@]} extra entries =="
  if [[ -n "${DQ8_S1_BLOCK:-}" ]]; then
    args+=(--s1-block "$DQ8_S1_BLOCK")
    echo "== S1b: emitting straight-line block $DQ8_S1_BLOCK =="
  fi
  if [[ -n "${DQ8_S1_CFG:-}" ]]; then
    # S1 rollout: DQ8_S1_CFG / DQ8_S1_CFG_END accept ONE entry..end pair or a
    # comma-separated LIST of parallel pairs (one per converted region).
    IFS=',' read -r -a s1_entries <<< "$DQ8_S1_CFG"
    IFS=',' read -r -a s1_ends <<< "${DQ8_S1_CFG_END:-}"
    if (( ${#s1_entries[@]} != ${#s1_ends[@]} )); then
      echo "DQ8_S1_CFG=$DQ8_S1_CFG needs ${#s1_entries[@]} comma-separated DQ8_S1_CFG_END value(s), got ${#s1_ends[@]}" >&2
      exit 2
    fi
    for s1_i in "${!s1_entries[@]}"; do
      args+=(--s1-cfg "${s1_entries[$s1_i]}" --s1-cfg-end "${s1_ends[$s1_i]}")
      echo "== S1c: emitting native CFG for region ${s1_entries[$s1_i]}..${s1_ends[$s1_i]} =="
    done
  fi
  cargo run --release --quiet -- recompile-arm "$CODE" 0x00100000 target/dq8-startup.c "${args[@]}"
  ls -l target/dq8-startup.c

  echo "== split dispatch =="
  split_args=()
  if [[ -n "${DQ8_OVERRIDES:-}" ]]; then
    split_args=(--overrides "$DQ8_OVERRIDES")
  fi
  python3 tools/split_dispatch.py target/dq8-startup.c "$SPLIT" "${split_args[@]}"
else
  echo "== regen skipped (DQ8_SPLIT_SKIP_REGEN=1): reusing existing $SPLIT/*.c =="
  [[ -f "$SPLIT/dq8_main.c" && -f "$SPLIT/dq8_rt.h" ]] || {
    echo "no split output in $SPLIT" >&2; exit 2; }
fi

echo "== compile ($JOBS jobs, cached by content hash) =="
# GL gate (from feature/gl-readback-fix): the experimental OpenGL rasterizer
# (src/pica_gl.c) is compiled in only when explicitly requested; the SDL
# window-surface presentation path above does not need it.
if [[ "${DQ8_GL:-0}" != "0" ]] && [[ -f /usr/include/EGL/egl.h ]] && \
   pkg-config --exists egl 2>/dev/null; then
  cflags+=(-DDQ8_ENABLE_GL)
  rcflags+=(-DDQ8_ENABLE_GL)
  # shellcheck disable=SC2207
  gate=($(pkg-config --cflags egl))
  cflags+=("${gate[@]}")
  rcflags+=("${gate[@]}")
  ldflags+=($(pkg-config --libs egl))
elif [[ "${DQ8_GL:-0}" == "1" ]]; then
  echo "DQ8_GL=1 requested but EGL development files not found (need pkg-config egl)" >&2
  exit 2
fi
echo "== flags: guest   \"${cflags[*]}\""
echo "== flags: runtime \"${rcflags[*]}\""
mkdir -p "$SPLIT/obj"
OBJDIR="$SPLIT/obj"
STATS="$OBJDIR/.build-stats"
: > "$STATS"

# Drop cached objects whose source part no longer exists (stale leftovers
# from a previous, larger manifest): they must not be compiled or linked.
pruned=0
for o in "$OBJDIR"/dq8_part_*.o "$OBJDIR"/dq8_part_*.o.stamp; do
  [[ -e "$o" ]] || continue
  base=$(basename "$o"); base=${base%.stamp}; base=${base%.o}
  if [[ ! -f "$SPLIT/$base.c" ]]; then
    echo "  prune    $base (no $base.c in manifest)"
    rm -f "$o"
    pruned=$((pruned + 1))
  fi
done

# Everything that must invalidate a cached .o is captured once here.
CLANG_ID=$(clang --version)
HDR_HASH=$(sha256sum "$SPLIT/dq8_rt.h" | cut -d' ' -f1)

# compile_cached <source.c> <target.o> <flag-array-name>
# Reuse <target.o> when its sidecar stamp still matches the current content
# hash; otherwise compile it and refresh the stamp atomically.  A failed
# compile deletes the object and its stamp so a stale .o can never be linked.
# The flag array to use is named by the third argument (cflags for guest
# parts, rcflags for the native runtime; both already carry the SDL/GL gates).
# The exact clang command line — including the flag set — is part of the cache
# key, so guest parts are never rebuilt because the runtime flags changed (or
# vice versa) and no stale object can survive a flag flip.
compile_cached() {
  local src=$1 obj=$2
  local -n flagset=$3
  local -a cmd=(clang "${flagset[@]}" -I"$SPLIT" -c "$src" -o "$obj")
  local base key
  base=$(basename "$obj")
  key=$(printf 'cmd=%s\nclang=%s\nhdr=%s\nsrc=%s\n' \
        "$(printf '%q ' "${cmd[@]}")" "$CLANG_ID" "$HDR_HASH" \
        "$(sha256sum "$src" | cut -d' ' -f1)" | sha256sum | cut -d' ' -f1)
  if [[ -f "$obj" && -f "$obj.stamp" && "$(<"$obj.stamp")" == "$key" ]]; then
    echo "  reuse    $base"
    printf 'reused %s\n' "$base" >> "$STATS"
    return 0
  fi
  echo "  compile  $base"
  if ! "${cmd[@]}"; then
    rm -f "$obj" "$obj.stamp"
    printf 'failed %s\n' "$base" >> "$STATS"
    return 1
  fi
  printf '%s\n' "$key" > "$obj.stamp.tmp"
  mv -f "$obj.stamp.tmp" "$obj.stamp"
  printf 'compiled %s\n' "$base" >> "$STATS"
}

compile_cached "$SPLIT/dq8_main.c" "$OBJDIR/dq8_main.o" rcflags
running=0
for part in "$SPLIT"/dq8_part_*.c; do
  base=$(basename "$part" .c)
  compile_cached "$part" "$OBJDIR/$base.o" cflags &
  running=$((running + 1))
  if (( running >= JOBS )); then wait -n || true; running=$((running - 1)); fi
done
wait

if grep -q '^failed ' "$STATS"; then
  echo "compile FAILED: $(grep '^failed ' "$STATS" | awk '{print $2}' | tr '\n' ' ')" >&2
  exit 1
fi
n_compiled=$(grep -c '^compiled ' "$STATS" || true)
n_reused=$(grep -c '^reused ' "$STATS" || true)
echo "== compile summary: $n_compiled compiled, $n_reused reused (of $((n_compiled + n_reused)) TUs) =="

echo "== link =="
clang "$SPLIT"/obj/*.o -o "$OUT" "${ldflags[@]}"
ls -l "$OUT"
echo "BUILD OK: $OUT"
echo "FLAGS OK: guest=\"$DFLAGS\" runtime=\"$RFLAGS\""
