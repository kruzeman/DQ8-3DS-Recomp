#!/usr/bin/env bash
# t_3de68379: per-stage frame-budget measurement (fps + guest-interpretation
# share) on the savestate scene.
#
# usage: tools/codegen_budget.sh <label> <binary> [duration_s] [warmup_s] [hz]
#
# Reuses the t_432a85ab frame-budget kit (reports/FRAMEBUDGET_t_432a85ab.md):
# it is the only proven way to attribute CPU on this host (perf_event_paranoid=4
# and yama ptrace_scope=1 block perf record / gdb -p; the sampler is the
# target's PARENT, which yama allows).  The two source scripts hardcode their
# evidence directory and binary name, so they are copied into target/ with
# those two strings rewritten - the originals are never modified.
#
# Output: evidence/t_3de68379/<label>/{run.log,samples.tsv,proc.tsv,summary.txt}
#   summary.txt: fps/ms-frame (FRAMESTAT+TIME), main-thread leaf shares with
#   the guest-interpretation buckets merged into one headline number.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

LAB=${1:?label}
BIN=${2:?binary}
DUR=${3:-60}
WARM=${4:-10}
HZ=${5:-5}

BINBASE=$(basename "$BIN")
case "$BIN" in /*) ;; *) BIN="$PWD/$BIN" ;; esac
WS="$PWD/evidence/t_3de68379"
OUT="$WS/$LAB"
KIT=tools/framebudget
# sed target: the kit's two hardcoded paths.
KIT_SAMPLE=target/cb_pysample.py
KIT_ANALYZE=target/cb_analyze.py

mkdir -p "$OUT"
# pysample.py writes to <ws>/evidence/<label>; repoint both so the run lands
# in $OUT directly (the kit's originals are never modified).
sed -e "s#ws = '/home/hermes/.hermes/kanban/workspaces/t_432a85ab'#ws = '$OUT'#" \
    -e "s#out = os.path.join(ws, 'evidence', lab)#out = ws#" \
    -e "s#binpath = '[^']*'#binpath = '$BIN'#" \
    "$KIT/pysample.py" > "$KIT_SAMPLE"
sed -e "s#base = os.path.join('/home/hermes/.hermes/kanban/workspaces/t_432a85ab/evidence', lab)#base = lab#" \
    -e "s#mod == 'dq8-texcache3'#mod == '$BINBASE'#" \
    -e "s#r\[2\] == 'dq8-texcache3'#r[2] == '$BINBASE'#" \
    "$KIT/analyze_samples.py" > "$KIT_ANALYZE"

echo "== sample: label=$LAB bin=$BINBASE dur=$DUR warm=$WARM hz=$HZ =="
python3 "$KIT_SAMPLE" "$LAB" "$DUR" "$WARM" "$HZ"

python3 "$KIT_ANALYZE" "$OUT" | tee "$OUT/summary.txt"
{
  echo
  echo "=== fps / frame time (DQ8_FRAME_STATS + DQ8_TRACE_TIME) ==="
  python3 "$KIT/extract_stats.py" "$OUT/run.log"
} | tee -a "$OUT/summary.txt"

echo "OUT=$OUT"
