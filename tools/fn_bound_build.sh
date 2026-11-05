#!/usr/bin/env bash
# Build both arms of the exact-set A/B (t_21dda9b5):
#   target/dq8-fnbound         exact spill/reload sets (this card's change)
#   target/dq8-fnbound-before  DQ8_FN_EXACT=0, i.e. dq8-b30's spill-all
set -uo pipefail
cd /home/hermes/DQ8/worktrees/fnbound-t_21dda9b5
export DQ8_GL=1 DQ8_OVERRIDES=tools/overrides/registry.json
export DQ8_FN_EMIT_LIST=tools/fn_emit_top300.txt BUILD_JOBS=12

echo "=== build after (exact sets) $(date -Is)"
t0=$(date +%s)
SPLIT_DIR=target/fnbound-split tools/lock_run.sh --kind build -- \
  tools/build_split.sh target/dq8-fnbound > target/build-after.log 2>&1
echo "rc=$? after build $(( $(date +%s) - t0 ))s"
tail -3 target/build-after.log

echo "=== build before (DQ8_FN_EXACT=0) $(date -Is)"
t0=$(date +%s)
DQ8_FN_EXACT=0 SPLIT_DIR=target/fnbound-split-before tools/lock_run.sh --kind build -- \
  tools/build_split.sh target/dq8-fnbound-before > target/build-before.log 2>&1
echo "rc=$? before build $(( $(date +%s) - t0 ))s"
tail -3 target/build-before.log
ls -l target/dq8-fnbound target/dq8-fnbound-before
