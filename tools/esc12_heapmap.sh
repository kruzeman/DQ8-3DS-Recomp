#!/usr/bin/env bash
# Round-12 heap-map capture: faithful round-9 repro (esc7-fresh-src save + the
# exact press schedule) with ControlMemory/CreateThread/thread-dump tracing, to
# reconstruct the guest's userspace heap and locate the first overlapping
# allocation (thread stack vs heap record).
set -u
cd /home/hermes/DQ8 || exit 1

LABEL=esc12-heapmap
DUR=400
BIN=./target/dq8-startup-split
UD_SRC=target/esc7-fresh-src
OUT=target/title-evidence/$LABEL
UD=$OUT/userdata

if [ ! -x "$BIN" ]; then echo "missing binary $BIN" >&2; exit 1; fi
if [ ! -d "$UD_SRC" ]; then echo "missing save src $UD_SRC" >&2; exit 1; fi
mkdir -p "$OUT"
if [ ! -d "$UD" ]; then cp -a "$UD_SRC" "$UD"; fi

INPUT=$OUT/input.txt
printf '0\n' > "$INPUT"
: > "$OUT/run.log"
: > "$OUT/timeline.log"

SPECS=(30 60 90 195 203 207 211 216:START 221:START 226 231:START 236 241 246 250 256 262 268 274 280 286 292 300 310 320 330 340 360 380)
BUTTON_A=0x1; BUTTON_START=0x8
HOLD_D=(); HOLD_U=(); HOLD_V=()
for spec in "${SPECS[@]}"; do
  btn=A; base=$spec
  case "$spec" in *:*) btn=${spec##*:}; base=${spec%%:*} ;; esac
  case "$btn" in START|start) value=$BUTTON_START ;; *) value=$BUTTON_A ;; esac
  case "$base" in *-*) d=${base%%-*}; u=${base##*-} ;; *) d=$base; u=$((base+1)) ;; esac
  HOLD_D+=("$d"); HOLD_U+=("$u"); HOLD_V+=("$value")
  echo "spec $spec -> hold [$d,$u) value=$value" >> "$OUT/timeline.log"
done

# Trace: threads (stacks), ControlMemory (heap regions), thread dump at exit.
env DQ8_HEADLESS=1 DQ8_DISPLAY=0 DQ8_RUN_GUEST_THREADS=1 \
    DQ8_TRACE_IPC=0 DQ8_TRACE_INPUT=1 DQ8_PROGRESS_SECONDS=10 \
    DQ8_TRACE_THREADS=1 DQ8_TRACE_THREAD_ID=1 \
    DQ8_USER_DATA_DIR="$UD" DQ8_INPUT_FILE="$INPUT" \
    "$BIN" DQ8.code target/dq8.romfs > "$OUT/run.log" 2>&1 &
GP=$!
echo "t0=$(date +%s.%N) pid=$GP label=$LABEL duration=$DUR" >> "$OUT/timeline.log"

for ((t=1; t<=DUR; t++)); do
  sleep 1
  if ! kill -0 "$GP" 2>/dev/null; then
    echo "t=$t process exited early" >> "$OUT/timeline.log"; break
  fi
  for i in "${!HOLD_D[@]}"; do
    if (( t == HOLD_D[i] )); then printf '%s\n' "${HOLD_V[i]}" > "$INPUT"; fi
    if (( t == HOLD_U[i] )); then printf '0\n' > "$INPUT"; fi
  done
done
kill -0 "$GP" 2>/dev/null && kill -KILL "$GP" 2>/dev/null
wait "$GP" 2>/dev/null
rc=$?
echo "exit=$rc t_end=$(date +%s.%N)" >> "$OUT/timeline.log"

echo "=== rc=$rc ==="
echo "=== ControlMemory calls ==="
grep -c "ControlMemory " "$OUT/run.log"
echo "=== CreateThread stacks ==="
grep "guest CreateThread" "$OUT/run.log" | head -40
echo "=== escapes ==="
grep "translated PC escaped\|guest memory fault" "$OUT/run.log" | head
