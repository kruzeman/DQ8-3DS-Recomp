#!/usr/bin/env bash
# hotreg/final2-e1775.sh -- t_e1773c51 step 1b proof on dq8-e1775 (fixed handshake).
#   5 x (300 s stress=2 city + 300 s stress=2 field) with DQ8_VSPAR_VERIFY=1
#   5 x 300 s town runs with a captured top screen (ppm_flat.py metric)
set -u
cd "$HOME/DQ8" || exit 2
OUT="$HOME/hotreg/final2-e1775"
mkdir -p "$OUT"
SUM="$OUT/summary.txt"
: > "$SUM"

note_run() {
  local tag=$1 dir=$2
  {
    echo "== $tag dir=$dir"
    echo "   freeze=$(grep -c 'FREEZE WATCHDOG' "$dir/err.log" 2>/dev/null || echo 0) lostwake=$(grep -c 'VSPARWAIT LOSTWAKE' "$dir/err.log" 2>/dev/null || echo 0) stall=$(grep -c 'VSPARWAIT STALL' "$dir/err.log" 2>/dev/null || echo 0)"
    grep 'VSPARSTRESS' "$dir/err.log" | tail -1
    grep 'VSPARVERIFY ' "$dir/err.log" | tail -1
    grep 'FRAMEPROF-SUM' "$dir/err.log" | tail -1
  } >> "$SUM"
}

stress() {
  local sc=$1 tag=$2
  taskset -c 0-7 bash "$HOME/rigrun.sh" dq8-e1775 "$sc" 300 "$tag" \
    DQ8_VS4_STRESS=2 DQ8_VSPAR_VERIFY=1 DQ8_VSPLAN_STATS=1 > "$OUT/$tag.rig.txt" 2>&1
  note_run "$tag" "$(sed -n 's/.*dir=\(\S*\).*/\1/p' "$OUT/$tag.rig.txt" | head -1)"
}

for i in 1 2 3 4 5; do
  stress city  "s$i-city"
  stress field "s$i-field"
done

for i in 1 2 3 4 5; do
  dd="$OUT/town-$i"; mkdir -p "$dd/ud"
  env DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_SPLIT_GL=0 \
      DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$dd" DQ8_FRAMEPROF=1 \
      DQ8_VBLANK_WALLCLOCK=1 DQ8_USER_DATA_DIR="$dd/ud" DQ8_VSPAR_VERIFY=1 \
      timeout -s KILL 300 ./dq8-e1775 --load-state "$HOME/hotreg/states/city.sav" \
        DQ8.code dq8.romfs > "$dd/out.log" 2> "$dd/err.log"
  echo -n "town-$i rc=$? " >> "$SUM"
  python3 "$HOME/hotreg/ppm_flat.py" "$dd/dq8-screen.ppm" >> "$SUM" 2>&1
  echo "   freeze=$(grep -c 'FREEZE WATCHDOG' "$dd/err.log" 2>/dev/null || echo 0)" >> "$SUM"
  grep 'VSPARVERIFY ' "$dd/err.log" | tail -1 >> "$SUM"
  grep 'FRAMEPROF-SUM' "$dd/err.log" | tail -1 >> "$SUM"
done
echo DONE >> "$SUM"
