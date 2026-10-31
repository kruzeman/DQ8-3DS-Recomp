#!/usr/bin/env bash
# Round-14 verification for t_eb3aef76 — strengthened layout fix:
#   - colliding stacks AND any stack with top inside a guest allocator region
#     (app heap / LINEAR) are relocated to private 64 KiB slots
#   - root 0x00206E6C (round-13 untranslated command-wrapper entry)
# Arms (400 s each, faithful round-9/11 press schedule, fresh esc7-fresh-src save):
#   fixon-a / fixon-b  fix ON (default)          — expect 0 escapes to t=400
#   noreloc            DQ8_STACK_NO_RELOCATE=1   — A/B raw-allocator control
#   fixwatch           fix ON + write-watch 0x08690300:0x100 + thread tracing
set -u
cd /home/hermes/DQ8 || exit 1

BIN=${ESC_BIN:-./target/dq8-stackfix13}
UD_SRC=target/esc7-fresh-src
DUR=${ESC_DUR:-400}
SPECS=(30 60 90 195 203 207 211 216:START 221:START 226 231:START 236 241 246 250 256 262 268 274 280 286 292 300 310 320 330 340 360 380)
BUTTON_A=0x1; BUTTON_START=0x8

[ -x "$BIN" ] || { echo "missing binary $BIN" >&2; exit 1; }
[ -d "$UD_SRC" ] || { echo "missing save src $UD_SRC" >&2; exit 1; }

ARMS=(fixon-a fixon-b noreloc fixwatch)
PREFIX=${ESC_PREFIX:-esc14}
for arm in "${ARMS[@]}"; do
  OUT=target/title-evidence/$PREFIX-$arm
  mkdir -p "$OUT"
  rm -rf "$OUT/userdata"; cp -a "$UD_SRC" "$OUT/userdata"
  printf '0\n' > "$OUT/input.txt"
  : > "$OUT/run.log"
  : > "$OUT/timeline.log"
done

launch() { # arm extra_env...
  local arm=$1; shift
  local OUT=target/title-evidence/$PREFIX-$arm
  local INPUT=$OUT/input.txt
  env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
      DQ8_PROGRESS_SECONDS=10 "$@" \
      DQ8_USER_DATA_DIR="$OUT/userdata" DQ8_INPUT_FILE="$INPUT" \
      "$BIN" DQ8.code target/dq8.romfs > "$OUT/run.log" 2>&1 &
  local gp=$!
  echo "t0=$(date +%s.%N) pid=$gp arm=$arm bin=$BIN" >> "$OUT/timeline.log"
  for ((t=1; t<=DUR; t++)); do
    sleep 1
    if ! kill -0 "$gp" 2>/dev/null; then
      echo "t=$t process exited early" >> "$OUT/timeline.log"; break
    fi
    for spec in "${SPECS[@]}"; do
      btn=A; base=$spec
      case "$spec" in *:*) btn=${spec##*:}; base=${spec%%:*} ;; esac
      case "$btn" in START|start) value=$BUTTON_START ;; *) value=$BUTTON_A ;; esac
      case "$base" in *-*) d=${base%%-*}; u=${base##*-} ;; *) d=$base; u=$((base+1)) ;; esac
      if (( t == d )); then printf '%s\n' "$value" > "$INPUT"; fi
      if (( t == u )); then printf '0\n' > "$INPUT"; fi
    done
  done
  kill -0 "$gp" 2>/dev/null && kill -KILL "$gp" 2>/dev/null
  wait "$gp" 2>/dev/null
  echo "exit=$? t_end=$(date +%s.%N)" >> "$OUT/timeline.log"
}

launch fixon-a &
launch fixon-b &
launch noreloc DQ8_STACK_NO_RELOCATE=1 &
launch fixwatch DQ8_TRACE_WRITE=0x08690300:0x100 DQ8_TRACE_THREADS=1 DQ8_TRACE_THREAD_ID=1 &
wait

for arm in "${ARMS[@]}"; do
  OUT=target/title-evidence/$PREFIX-$arm
  echo "=== $PREFIX-$arm ==="
  echo -n "escapes/faults: "; grep -c "translated PC escaped\|guest memory fault" "$OUT/run.log"
  echo -n "relocations: "; grep -c "thread-stack RELOCATE" "$OUT/run.log"
  echo -n "overlaps/overflows: "; grep -c "thread-stack OVERLAP\|stack OVERFLOW" "$OUT/run.log"
  grep -m2 "thread-stack RELOCATE\|NO_RELOCATE\|allocator region" "$OUT/run.log"
  tail -2 "$OUT/timeline.log"
done
