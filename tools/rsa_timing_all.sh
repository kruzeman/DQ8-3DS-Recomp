#!/usr/bin/env bash
# t_05281b89 stage A: the whole interleaved gl_ms battery (field then battle).
set -u
B=${1:-/home/hermes/.hermes/profiles/renderer/cache/scratch/rsa}
W=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$W/tools/rsa_glms.sh" "$B/glms_field" \
    /home/hermes/DQ8/evidence/render-cape-path/dq8-state.sav 8000000 3 \
    > "$B/glms_field.txt" 2>&1
"$W/tools/rsa_glms.sh" "$B/glms_battle" \
    /home/hermes/DQ8/evidence/hot-regions-20261102/state-battle.sav 8000000 3 \
    > "$B/glms_battle.txt" 2>&1
echo DONE > "$B/glms_done.txt"
