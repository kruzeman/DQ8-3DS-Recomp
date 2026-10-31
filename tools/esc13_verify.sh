#!/usr/bin/env bash
# Round-13 verification for t_eb3aef76 (thread-stack/heap aliasing layout fix).
# Mirrors the proven esc9-rootsmoke_400 repro (fresh save + faithful press
# schedule, 400 s, minimal tracing) and runs four arms in parallel:
#   fixon-a   fix ON  (relocation enabled, default)      — expect 0 escapes
#   fixon-b   fix ON  repeat (determinism)               — expect 0 escapes
#   noreloc   fix OFF (DQ8_STACK_NO_RELOCATE=1)          — A/B control, escape should recur
#   fixwatch  fix ON + write-watch 0x08690300:0x100      — aliasing-writer proof
set -u
cd /home/hermes/DQ8 || exit 1

BIN=${ESC_BIN:-./target/dq8-startup-split}
UD_SRC=${ESC_UD_SRC:-target/esc7-fresh-src}
DUR=${ESC_DUR:-400}
SPECS=(30 60 90 195 203 207 211 216:START 221:START 226 231:START 236 241 246 250 256 262 268 274 280 286 292 300 310 320 330 340 360 380)
BUTTON_A=0x1; BUTTON_START=0x8

[ -x "$BIN" ] || { echo "missing binary $BIN" >&2; exit 1; }
[ -d "$UD_SRC" ] || { echo "missing save src $UD_SRC" >&2; exit 1; }

ARMS=(fixon-a fixon-b noreloc fixwatch)
PREFIX=${ESC_PREFIX:-esc13}
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
  {
    for spec in "${SPECS[@]}"; do
      btn=A; base=$spec
      case "$spec" in *:*) btn=${spec##*:}; base=${spec%%:*} ;; esac
      case "$btn" in START|start) value=$BUTTON_START ;; *) value=$BUTTON_A ;; esac
      echo "spec $spec value=$value"
    done
  } > "$OUT/specs.log"
  env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
      DQ8_PROGRESS_SECONDS=10 "$@" \
      DQ8_USER_DATA_DIR="$OUT/userdata" DQ8_INPUT_FILE="$INPUT" \
      "$BIN" DQ8.code target/dq8.romfs > "$OUT/run.log" 2>&1 &
  local gp=$!
  echo "t0=$(date +%s.%N) pid=$gp arm=$arm" >> "$OUT/timeline.log"
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
  grep -c "translated PC escaped\|guest memory fault" "$OUT/run.log"
  grep -m2 "thread-stack RELOCATE\|NO_RELOCATE" "$OUT/run.log"
  tail -2 "$OUT/timeline.log"
done
