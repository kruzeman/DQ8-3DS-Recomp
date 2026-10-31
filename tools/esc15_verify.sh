#!/usr/bin/env bash
# Round-15 ACCEPTANCE matrix for t_eb3aef76 — strengthened layout fix + 391
# literal-callback roots, on target/dq8-stackfix13 (build 17:11).
# Same faithful round-9/11 repro as esc9-rootsmoke_400; adds PPM capture with
# 5 s snapshots (tNNNN.ppm, same naming/cadence as the pre-fix baseline) for
# frame identity, and prints the acceptance greps at the end.
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
PREFIX=${ESC_PREFIX:-esc15}
for arm in "${ARMS[@]}"; do
  OUT=target/title-evidence/$PREFIX-$arm
  mkdir -p "$OUT/frames"
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
      DQ8_PROGRESS_SECONDS=10 DQ8_CAPTURE_PPM=1 \
      DQ8_CAPTURE_PPM_DIR="$OUT/frames" "$@" \
      DQ8_USER_DATA_DIR="$OUT/userdata" DQ8_INPUT_FILE="$INPUT" \
      "$BIN" DQ8.code target/dq8.romfs > "$OUT/run.log" 2>&1 &
  local gp=$!
  echo "t0=$(date +%s.%N) pid=$gp arm=$arm bin=$BIN" >> "$OUT/timeline.log"
  for ((t=1; t<=DUR; t++)); do
    sleep 1
    if ! kill -0 "$gp" 2>/dev/null; then
      echo "t=$t process exited early" >> "$OUT/timeline.log"; break
    fi
    # 5 s frame snapshots (same naming as the pre-fix baseline runs)
    if (( t % 5 == 0 )); then
      if [ -f "$OUT/frames/dq8-screen.ppm" ]; then
        cp -f "$OUT/frames/dq8-screen.ppm" "$OUT/frames/t$(printf '%04d' "$t").ppm" 2>/dev/null || true
      fi
      if [ -f "$OUT/frames/dq8-screen-bottom.ppm" ]; then
        cp -f "$OUT/frames/dq8-screen-bottom.ppm" "$OUT/frames/t$(printf '%04d' "$t").bottom.ppm" 2>/dev/null || true
      fi
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
  echo -n "round-9 sig 005CB6E0: "; grep -c "escaped to 005CB6E0" "$OUT/run.log"
  echo -n "round-11 sig pc=0/lr=00157854: "; grep -c "escaped to 00000000\|lr=00157854" "$OUT/run.log"
  echo -n "relocations: "; grep -c "thread-stack RELOCATE" "$OUT/run.log"
  echo -n "overlaps/overflows: "; grep -c "thread-stack OVERLAP\|stack OVERFLOW" "$OUT/run.log"
  echo -n "ControlMemory: "; grep -c "ControlMemory" "$OUT/run.log"
  echo -n "frames: "; ls "$OUT/frames"/t*.ppm 2>/dev/null | wc -l
  echo -n "progress marks: "; grep -c "step=\|progress" "$OUT/run.log" 2>/dev/null || true
  tail -2 "$OUT/timeline.log"
done
