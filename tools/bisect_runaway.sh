#!/usr/bin/env bash
# Iteratively find and drop entries from a candidate list that cause recompile
# failure (runaway "no supported BX LR", "store with PC base", etc.).  Each
# recompile_try run fails on the FIRST bad entry it hits; we binary-search which
# candidate causes it and drop it.  Repeats until clean.
# usage: tools/bisect_runaway.sh CAND_FILE OUT_CLEAN_FILE
set -u
cd /home/hermes/DQ8 || exit 1
CAND=$1
OUT=$2

fails() { # res -> 0 if this recompile result is a failure
  echo "$1" | grep -qE 'no supported BX LR|store with PC base|recompile exit=[^0]'
}

cp "$CAND" /tmp/bisect_work.txt
round=0
while :; do
  round=$((round+1))
  n=$(grep -c . /tmp/bisect_work.txt)
  [ "$n" -eq 0 ] && { echo "EMPTY"; break; }
  res=$(bash tools/recompile_try.sh /tmp/bisect_work.txt /tmp/bisect_try.c 2>&1)
  if fails "$res"; then
    bad=$(echo "$res" | grep -oE '(function 0x[0-9A-F]+|at 0x[0-9A-F]+)' | head -1 | grep -oE '0x[0-9A-F]+')
    echo "round=$round failure at ${bad:-?} (n=$n): $(echo "$res" | grep -E 'no supported|store with' | head -1)"
    # binary-search minimal failing prefix
    lo=1; hi=$n
    while [ $lo -lt $hi ]; do
      mid=$(( (lo+hi)/2 ))
      head -n "$mid" /tmp/bisect_work.txt > /tmp/bisect_half.txt
      res2=$(bash tools/recompile_try.sh /tmp/bisect_half.txt /tmp/bisect_half.c 2>&1)
      if fails "$res2"; then hi=$mid; else lo=$((mid+1)); fi
    done
    culprit=$(sed -n "${lo}p" /tmp/bisect_work.txt)
    echo "  culprit candidate: $culprit (line $lo of $n)"
    grep -vxF "$culprit" /tmp/bisect_work.txt > /tmp/bisect_next.txt
    mv /tmp/bisect_next.txt /tmp/bisect_work.txt
  else
    echo "CLEAN after $((round-1)) drops (n=$n)"
    echo "$res" | tail -2
    break
  fi
  [ "$round" -gt 80 ] && { echo "TOO MANY ROUNDS"; break; }
done
cp /tmp/bisect_work.txt "$OUT"
echo "wrote clean list ($(grep -c . "$OUT")) to $OUT"
