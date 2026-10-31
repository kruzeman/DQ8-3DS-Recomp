#!/usr/bin/env bash
# ab_matrix.sh (t_86b8f1f9) — sequential A/B scenario cells on the frozen
# save-dialog scenario (tools/run_gl_matrix.py -> run_idle_scenario.py).
# usage per cell: run_cell <name> <sw|gl> <bin> [ENV=val ...]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.."
LOG=target/ab-matrix.log
: > "$LOG"

run_cell() {
  local name=$1 renderer=$2 bin=$3; shift 3
  echo "=== CELL $name renderer=$renderer bin=$bin ($(date +%H:%M:%S)) env: $* ===" >> "$LOG"
  env "$@" python3 tools/run_gl_matrix.py "$name" --renderer "$renderer" \
      --bin "$bin" --capture on --max-wall 200 >> "$LOG" 2>&1
  echo "=== CELL $name done rc=$? ($(date +%H:%M:%S)) ===" >> "$LOG"
}

# 1) AFTER: new binary, GL, texture cache + context hold on (defaults).
run_cell t86-after-gl gl target/dq8-tex-new \
  DQ8_FRAME_STATS=1 DQ8_GL_TEXSTATS=1

# 2) OLD SEMANTICS: new binary + kill switches (per-draw staging decode +
#    per-draw context displacement) -> instrumented "before" counters.
run_cell t86-oldsem-gl gl target/dq8-tex-new \
  DQ8_FRAME_STATS=1 DQ8_GL_TEXSTATS=1 DQ8_GL_TEXCACHE=0 DQ8_GL_CTX_HOLD=0

# 3) BEFORE: pre-change binary, GL (frame checkpoints + CPU reference).
run_cell t86-base-gl gl target/dq8-tex-base

# 4) AFTER, software mode: FRAMESTAT redundancy in the SW rasterizer.
run_cell t86-after-sw sw target/dq8-tex-new \
  DQ8_FRAME_STATS=1

echo "ALL CELLS DONE $(date +%H:%M:%S)" >> "$LOG"
