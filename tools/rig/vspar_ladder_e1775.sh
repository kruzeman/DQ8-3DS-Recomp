#!/usr/bin/env bash
# hotreg/vspar-ladder-e1775.sh -- step-1b handshake at the other pool widths:
#   city @ DQ8_VSPAR=1 and 4, battle @ 8, 300 s each, per-vertex verification.
set -u
cd "$HOME/DQ8" || exit 2
OUT="$HOME/hotreg/ladder-e1775"
mkdir -p "$OUT"
SUM="$OUT/summary.txt"
: > "$SUM"
run() {
  local sc=$1 nw=$2 tag=$3
  taskset -c 0-7 bash "$HOME/rigrun.sh" dq8-e1775 "$sc" 300 "$tag" \
    DQ8_VSPAR="$nw" DQ8_VSPAR_VERIFY=1 > "$OUT/$tag.rig.txt" 2>&1
  local dir
  dir=$(sed -n 's/.*dir=\(\S*\).*/\1/p' "$OUT/$tag.rig.txt" | head -1)
  {
    echo "== $tag scene=$sc nw=$nw dir=$dir"
    echo "   freeze=$(grep -c 'FREEZE WATCHDOG' "$dir/err.log" 2>/dev/null || echo 0)"
    grep 'VSPARVERIFY ' "$dir/err.log" | tail -1
    grep 'VSPARSTAT ' "$dir/err.log" | tail -1
    grep 'FRAMEPROF-SUM' "$dir/err.log" | tail -1
  } >> "$SUM"
}
run city 1 l-city-v1
run city 4 l-city-v4
run battle 8 l-battle-v8
echo DONE >> "$SUM"
