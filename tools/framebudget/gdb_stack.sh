#!/usr/bin/env bash
# t_432a85ab: stack sampling under gdb (gdb is the PARENT of the target, so
# yama ptrace_scope=1 allows it).  SIGALRM is re-armed with alarm() at every
# stop; DQ8_PROGRESS_SECONDS must stay unset (the binary claims SIGALRM then).
# usage: gdb_stack.sh <label> <n_samples> <warmup_s>
set -u
WS=/home/hermes/.hermes/kanban/workspaces/t_432a85ab
WT=/home/hermes/DQ8/worktrees/texcache2-t_66c89cf2
BIN=$WT/target/dq8-texcache3
CODE=/home/hermes/DQ8/DQ8.code
ROMFS=/home/hermes/DQ8/target/dq8.romfs
STATE=/home/hermes/DQ8/saves/dq8-state.sav
HONEY=/home/hermes/DQ8/saves/honeySave.bin
LAB=${1:?label}
N=${2:?samples}
WARM=${3:-15}

OUT=$WS/evidence/$LAB
mkdir -p "$OUT/ud/archive-00000004-00000000"
cp -f "$HONEY" "$OUT/ud/archive-00000004-00000000/honeySave.bin"
printf '0\n' > "$OUT/input.txt"

ARGS=(-q -batch
  -ex 'set pagination off'
  -ex 'set confirm off'
  -ex 'set print thread-events off'
  -ex 'handle SIGALRM stop print nopass'
  -ex "file $BIN"
  -ex "set args $CODE $ROMFS --load-state $STATE"
  -ex 'cd '"$WT"
  -ex 'set env DQ8_HEADLESS=1'
  -ex 'set env DQ8_RUN_GUEST_THREADS=1'
  -ex 'set env DQ8_FRAME_STATS=1'
  -ex 'set env DQ8_TRACE_TIME=1'
  -ex 'set env DQ8_GL=1'
  -ex "set env DQ8_USER_DATA_DIR=$OUT/ud"
  -ex "set env DQ8_INPUT_FILE=$OUT/input.txt"
  -ex 'start'
  -ex "call (unsigned int)alarm($WARM)"
  -ex 'continue'
  -ex 'printf "=== WARM DONE ===\n"')

for ((i=1; i<=N; i++)); do
  ARGS+=(-ex 'call (unsigned int)alarm(1)')
  ARGS+=(-ex 'continue')
  ARGS+=(-ex "printf \"=== SAMPLE %d ===\\n\", $i")
  ARGS+=(-ex 'thread apply all bt 14')
done
ARGS+=(-ex 'kill')

cd "$WT" || exit 1
unset DQ8_PROGRESS_SECONDS
start=$(date +%s.%N)
gdb "${ARGS[@]}" > "$OUT/gdb.log" 2>&1
end=$(date +%s.%N)
echo "log=$OUT/gdb.log lines=$(wc -l < "$OUT/gdb.log") samples=$(grep -c '=== SAMPLE' "$OUT/gdb.log") elapsed=$(echo "$end - $start" | bc)s"
