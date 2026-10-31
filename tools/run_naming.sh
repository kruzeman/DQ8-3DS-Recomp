#!/usr/bin/env bash
# usage: run_naming.sh <tag> <gl:0|1> <seconds> ["<t>:<A|S|B> <t>:<A|S|B> ..."]
# Walk boot -> save dialog -> title -> menu -> multi-window NAMING screen with
# 1000 ms button holds (DQ8_INPUT_FILE takes a uint32 mask: A=1, B=2, START=8).
# Fresh userdata so the save dialog shows.  Verifies the f826f3f "naming screen
# in front of the save menu" win still holds after the GL vertex-layout fix.
set -u
cd "$(dirname "$0")/.."
TAG=$1; GL=$2; SECS=${3:-80}
SPECS=${4:-"12:A 20:S 26:A 32:S 38:A 44:S 50:A 56:S 62:A"}
DIR=/home/hermes/.hermes/kanban/workspaces/t_667c2f8b/caps8/$TAG
mkdir -p "$DIR/frames" "$DIR/userdata"
: > "$DIR/run.log"
INPUT=$DIR/input.txt
printf '0\n' > "$INPUT"
export DQ8_RUN_GUEST_THREADS=1 DQ8_HEADLESS=1 DQ8_SKIP_MOVIE=1
export DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$DIR"
export DQ8_USER_DATA_DIR="$DIR/userdata"
export DQ8_INPUT_FILE="$INPUT" DQ8_TRACE_INPUT=1
export DQ8_TRACE_LAYER=1
[ "$GL" = "1" ] && export DQ8_GL=1 || unset DQ8_GL
./target/dq8-gl DQ8.code dq8.romfs >> "$DIR/run.log" 2>&1 &
GP=$!
echo "t=0 start gl=$GL specs='$SPECS'" > "$DIR/timeline.log"
mask() {
  case "$1" in
    A) printf '1' ;;  B) printf '2' ;;  S) printf '8' ;;
    AS|SA) printf '9' ;;  *) printf '%s' "$1" ;;
  esac
}
i=0
while kill -0 $GP 2>/dev/null; do
  sleep 1
  i=$((i+1))
  active=""
  for spec in $SPECS; do
    t=${spec%%:*}; b=${spec##*:}
    [ "$i" -eq "$t" ] && active="$active $b"
  done
  if [ -n "$active" ]; then
    m=0
    for b in $active; do m=$(( m | $(mask "$b") )); done
    printf '%s\n' "$m" > "$INPUT"
    echo "t=$i DOWN=$active mask=$m" >> "$DIR/timeline.log"
    sleep 1
    printf '0\n' > "$INPUT"
    echo "t=$i UP" >> "$DIR/timeline.log"
  fi
  cp "$DIR/dq8-screen.ppm" "$DIR/frames/f_$(printf %03d $i).ppm" 2>/dev/null || true
  cp "$DIR/dq8-screen-bottom.ppm" "$DIR/frames/b_$(printf %03d $i).ppm" 2>/dev/null || true
  [ "$i" -ge "$SECS" ] && break
done
printf '0\n' > "$INPUT"
kill -9 $GP 2>/dev/null; wait $GP 2>/dev/null
echo "done $TAG top=$(ls "$DIR"/frames/f_*.ppm 2>/dev/null | wc -l) bot=$(ls "$DIR"/frames/b_*.ppm 2>/dev/null | wc -l)"
