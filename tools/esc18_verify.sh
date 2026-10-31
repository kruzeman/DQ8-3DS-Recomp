#!/usr/bin/env bash
# Round-12 (t_f0271792): LOAD-EXISTING-SAVE scenario — the surface past the
# fixed flows. Same faithful round-9/11 press schedule as esc15_verify.sh, but
# the userdata source carries a real (already-created) save, so the guest takes
# the "save exists -> read it back" path instead of the format/create path.
#
# Runs two arms in parallel for a clean A/B on the SAME scenario:
#   prefix-pre   = ./target/dq8-stackfix13 (built 17:23, BEFORE roots 3448dc0)
#   prefix-post  = $ESC_POST_BIN          (built AFTER roots 3448dc0)
#
# usage: ESC_PREFIX=esc18 ESC_POST_BIN=./target/dq8-esc12 bash tools/esc18_verify.sh
set -u
cd /home/hermes/DQ8 || exit 1

PRE_BIN=${ESC_PRE_BIN:-./target/dq8-stackfix13}
POST_BIN=${ESC_POST_BIN:-./target/dq8-esc12}
UD_SRC=${ESC_UD_SRC:-target/esc12-loadsrc}
DUR=${ESC_DUR:-400}
SPECS=(30 60 90 195 203 207 211:START 216:START 221 226 231:START 236 241 246 250 256 262 268 274 280 286 292 300 310 320 330 340 360 380)
BUTTON_A=0x1; BUTTON_START=0x8
PREFIX=${ESC_PREFIX:-esc18}

[ -d "$UD_SRC" ] || { echo "missing save src $UD_SRC" >&2; exit 1; }
[ -f "$UD_SRC/archive-00000004-00000000/honeySave.bin" ] || {
  echo "src $UD_SRC has no honeySave.bin (would be the fresh/create path)" >&2; exit 1; }

ARMS=(${ESC_ARMS:-pre post postb})
# Optional extra env (e.g. ESC_EXTRA_ENV="DQ8_TRACE_FS=1") for every arm.
EXTRA_ENV=()
[[ -n "${ESC_EXTRA_ENV:-}" ]] && read -ra EXTRA_ENV <<< "$ESC_EXTRA_ENV"
for arm in "${ARMS[@]}"; do
  OUT=target/title-evidence/$PREFIX-$arm
  mkdir -p "$OUT/frames"
  rm -rf "$OUT/userdata"; cp -a "$UD_SRC" "$OUT/userdata"
  printf '0\n' > "$OUT/input.txt"
  : > "$OUT/run.log"
  : > "$OUT/timeline.log"
done

launch() { # arm bin [extra env...]
  local arm=$1 bin=$2; shift 2
  local OUT=target/title-evidence/$PREFIX-$arm
  local INPUT=$OUT/input.txt
  env DQ8_HEADLESS=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1 \
      DQ8_PROGRESS_SECONDS=10 DQ8_CAPTURE_PPM=1 \
      DQ8_CAPTURE_PPM_DIR="$OUT/frames" "${EXTRA_ENV[@]}" "$@" \
      DQ8_USER_DATA_DIR="$OUT/userdata" DQ8_INPUT_FILE="$INPUT" \
      "$bin" DQ8.code target/dq8.romfs > "$OUT/run.log" 2>&1 &
  local gp=$!
  echo "t0=$(date +%s.%N) pid=$gp arm=$arm bin=$bin" >> "$OUT/timeline.log"
  for ((t=1; t<=DUR; t++)); do
    sleep 1
    if ! kill -0 "$gp" 2>/dev/null; then
      echo "t=$t process exited early" >> "$OUT/timeline.log"; break
    fi
    if (( t % 5 == 0 )); then
      [ -f "$OUT/frames/dq8-screen.ppm" ] &&
        cp -f "$OUT/frames/dq8-screen.ppm" "$OUT/frames/t$(printf '%04d' "$t").ppm" 2>/dev/null
      [ -f "$OUT/frames/dq8-screen-bottom.ppm" ] &&
        cp -f "$OUT/frames/dq8-screen-bottom.ppm" "$OUT/frames/t$(printf '%04d' "$t").bottom.ppm" 2>/dev/null
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

for arm in "${ARMS[@]}"; do
  case "$arm" in
    pre) launch pre "$PRE_BIN" & ;;
    post|postb) launch "$arm" "$POST_BIN" & ;;
    *) echo "unknown arm '$arm' (want pre|post|postb)" >&2; exit 1 ;;
  esac
done
wait

for arm in "${ARMS[@]}"; do
  OUT=target/title-evidence/$PREFIX-$arm
  echo "=== $PREFIX-$arm ==="
  echo -n "escapes/faults: "; grep -c "translated PC escaped\|guest memory fault" "$OUT/run.log"
  echo -n "  of which 00244850: "; grep -c "escaped to 00244850" "$OUT/run.log"
  grep "translated PC escaped\|guest memory fault" "$OUT/run.log" | head -3
  echo -n "stack overlaps/overflows: "; grep -c "thread-stack OVERLAP\|stack OVERFLOW" "$OUT/run.log"
  echo -n "relocations: "; grep -c "thread-stack RELOCATE" "$OUT/run.log"
  echo -n "hid press/release: "; grep -c "pressed=\|released=" "$OUT/run.log"
  echo -n "fs save ops (Format/Write/Open honeySave): "; grep -c "FormatSaveData\|honeySave" "$OUT/run.log"
  echo -n "frames: "; ls "$OUT/frames"/t*.ppm 2>/dev/null | wc -l
  tail -2 "$OUT/timeline.log"
done
