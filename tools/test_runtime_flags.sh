#!/usr/bin/env bash
# Correctness matrix for the split runtime/guest flag sets (audit PERF-03).
#
# The split build compiles the native runtime TU (dq8_main.c) with
# DQ8_SPLIT_RUNTIME_CFLAGS (default -O2) and the 37 guest dispatch TUs with
# DQ8_SPLIT_CFLAGS (default -O0, unchanged).  This script proves that the
# runtime optimisation level does not change behaviour:
#
#   * suites that already compile every case at -O0 AND -O2 internally
#     (and require identical output) are run once: they are their own
#     runtime/guest differential;
#   * suites that hard-code -O0 are run twice: once as shipped, once through a
#     `clang`/`cc` shim that appends -O2 (last -O wins in clang), so the same
#     harness source and the same generated guest C are built at both runtime
#     optimisation levels.  Both arms assert absolute expected values, so
#     "both pass with identical output" is the differential.
#
# usage: tools/test_runtime_flags.sh [outdir]        (default reports/runtime-flags)
# Env:   DQ8_TEST_JOBS  parallel clang jobs for the big matrices (default 8)
#        DQ8_TOOL       dq8tool path (default target/release/dq8tool)
# Exit 0 only when every arm passes and the two arms agree.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

OUT=${1:-reports/runtime-flags}
JOBS=${DQ8_TEST_JOBS:-8}
export DQ8_TOOL=${DQ8_TOOL:-$PWD/target/release/dq8tool}
export DQ8_RUN_GUEST_THREADS=1
[[ -x $DQ8_TOOL ]] || { echo "missing $DQ8_TOOL (cargo build --release)" >&2; exit 2; }
mkdir -p "$OUT"

# clang shim that appends one optimisation flag: the suites below pass -O0
# explicitly, and clang honours the LAST -O on the command line.
SHIM=$OUT/shim-o2
mkdir -p "$SHIM"
for prog in clang cc; do
  cat > "$SHIM/$prog" <<'EOS'
#!/bin/sh
# append -O2 so a hard-coded -O0 build is re-evaluated at the runtime level
exec /usr/bin/clang "$@" -O2
EOS
  chmod +x "$SHIM/$prog"
done

fail=0
run() { # run <label> <logfile> <cmd...>
  local label=$1 log=$2; shift 2
  echo "== $label"
  if "$@" >"$log" 2>&1; then
    echo "   PASS  ($log)"
  else
    echo "   FAIL  rc=$? ($log)"; fail=$((fail + 1))
  fi
}

# ---- suites with an internal O0/O2 (and UBSan) differential ----------------
run "arm_flags      O0 vs O2 vs UBSan, 489 cases" "$OUT/arm_flags.log" \
    python3 tools/test_arm_flags.py --jobs "$JOBS"
run "vfp_arith      O0 vs O2 vs oracle vs split" "$OUT/vfp_arith.log" \
    python3 tools/test_vfp_arith.py --jobs "$JOBS"
run "split_hooks    single vs split, O0 vs O2 (A05)" "$OUT/split_hooks.log" \
    python3 tools/test_split_hooks.py
run "tex_decode     O0 vs O2 vs azahar" "$OUT/tex_decode.log" \
    python3 tools/test_tex_decode.py
run "kernel_sync    O0 vs O2 contract differential" "$OUT/kernel_sync_contract.log" \
    python3 tools/test_kernel_sync_contract.py

# ---- suites that hard-code one opt level: run at O0 and at O2 --------------
for spec in "kernel_waits:tools/test_kernel_waits.py" \
            "native_fs:tools/test_native_fs.py" \
            "native_fs_flush:tools/test_native_fs_flush.py"; do
  name=${spec%%:*}; script=${spec#*:}
  run "$name runtime-O0" "$OUT/$name.O0.log" python3 "$script"
  # O2 arm: the shim is first on PATH, so the harness C is compiled at -O2.
  echo "== $name runtime-O2 (clang shim)"
  if PATH="$SHIM:$PATH" python3 "$script" >"$OUT/$name.O2.log" 2>&1; then
    echo "   PASS  ($OUT/$name.O2.log)"
  else
    echo "   FAIL  ($OUT/$name.O2.log)"; fail=$((fail + 1))
  fi
done

echo
echo "== summaries"
for f in "$OUT"/*.log; do
  printf '%-34s %s\n' "$(basename "$f")" "$(grep -E '^(OK|FAILED \(|Ran [0-9]+ test|ok$|all scenarios)' "$f" | tr '\n' ' ' | cut -c1-90)"
done
echo
if (( fail )); then echo "MATRIX FAILED: $fail arm(s)"; exit 1; fi
echo "MATRIX OK: all arms passed"
