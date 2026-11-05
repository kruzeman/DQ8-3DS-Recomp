#!/usr/bin/env bash
# tools/fnhook_battle_ab.sh -- battle-only, b35 vs this build, 5 interleaved
# repetitions, for the t_e1547d93 verdict.  Same metric as fnhook_ab.sh.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
SECS=${SECS:-25}
REPS=${REPS:-5}
OUT=${OUT:-target/fnhook-ab-battle}
NEW=${NEW:-target/dq8-fnhook2}
B35=${B35:-/home/hermes/DQ8/worktrees/integ/target/dq8-b35}
BATTLE=${BATTLE:-/home/hermes/DQ8/evidence/hot-regions-20261102/state-battle.sav}
BASE=(DQ8_HEADLESS=1 DQ8_GL=1 DQ8_RUN_GUEST_THREADS=1 DQ8_VSPAR=4 DQ8_SPLIT_GL=0
      DQ8_STACK_NO_RELOCATE=1 DQ8_GL_DEPTH_ATTACH=1 DQ8_FRAMEPROF=1)
mkdir -p "$OUT"
: > "$OUT/runs.tsv"
run() {
  local bin=$1 gate=$2 tag=$3
  env "${BASE[@]}" DQ8_FN_EMIT="$gate" DQ8_USER_DATA_DIR="$PWD/$OUT/ud-$tag" \
    timeout -k 10 "$SECS" "$bin" --load-state "$BATTLE" DQ8.code dq8.romfs \
    > "$OUT/battle-$tag-$rep.out" 2> "$OUT/battle-$tag-$rep.err"
  local rc=$?
  local mean
  mean=$(awk '/^FRAMEPROF f=/{g="";t="";for(i=1;i<=NF;i++){if($i~/^guest_ms=/){split($i,x,"=");g=x[2]}if($i~/^total_ms=/){split($i,x,"=");t=x[2]}}if(t+0>2.0){s+=g;n++}}END{if(n)printf "%.3f",s/n;else printf "nan"}' "$OUT/battle-$tag-$rep.err")
  local frames fault
  frames=$(grep -c '^FRAMEPROF f=' "$OUT/battle-$tag-$rep.err")
  fault=$(grep -c 'guest memory fault' "$OUT/battle-$tag-$rep.err")
  printf "battle\t%s\t%s\t%s\t%s\t%s\n" "$tag" "$rep" "$mean" "$frames" "$fault" >> "$OUT/runs.tsv"
  echo "  battle $tag rep$rep guest_ms=$mean frames=$frames fault=$fault"
}
for rep in $(seq 1 "$REPS"); do
  run "$B35" all b35
  run "$NEW" all new2800
done
python3 - "$OUT/runs.tsv" <<'PY'
import sys, statistics as st
rows = {}
for l in open(sys.argv[1]):
    s, tag, rep, mean, frames, fault = l.split()
    rows.setdefault(tag, []).append(float(mean))
for tag, v in rows.items():
    print("%-8s n=%d min=%.3f mean=%.3f max=%.3f" % (tag, len(v), min(v), st.mean(v), max(v)))
a, b = rows.get("b35", []), rows.get("new2800", [])
if a and b:
    print("b35 -> new2800: %+.1f %% (%+.3f ms), ranges %s" % (
        100.0 * (st.mean(b) - st.mean(a)) / st.mean(a), st.mean(b) - st.mean(a),
        "disjoint" if max(b) < min(a) else "overlap"))
PY
