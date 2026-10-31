#!/usr/bin/env bash
# t_432a85ab: per-thread CPU + state + wchan sampler (perf is blocked).
# usage: tsample.sh <pid> <interval_s> <duration_s> <out_file>
set -u
PID=${1:?pid}
IV=${2:?interval}
DUR=${3:?duration}
OUT=${4:?out}
: > "$OUT"
end=$(( $(date +%s) + DUR ))
while [ "$(date +%s)" -lt "$end" ]; do
  ts=$(date +%s.%N)
  for s in /proc/$PID/task/*; do
    [ -r "$s/stat" ] || continue
    tid=${s##*/}
    # comm may contain spaces/parens: strip "pid (comm)" prefix, then fields
    rest=$(sed -E 's/^[0-9]+ \(.*\) //' "$s/stat" 2>/dev/null) || continue
    # after strip: field1=state(3) ... utime=field12 stime=field13
    state=$(echo "$rest" | awk '{print $1}')
    ut=$(echo "$rest" | awk '{print $12}')
    st=$(echo "$rest" | awk '{print $13}')
    wch=$(cat "$s/wchan" 2>/dev/null)
    comm=$(sed -E 's/^[0-9]+ \((.*)\) .*/\1/' "$s/stat" 2>/dev/null)
    printf '%s %s %s %s %s %s\n' "$ts" "$tid" "$comm" "${ut:-?}" "${st:-?}" "${wch:-?}" >> "$OUT"
  done
  sleep "$IV"
done
echo "sample loop done" >> "$OUT"
