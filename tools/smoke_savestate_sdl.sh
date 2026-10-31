#!/usr/bin/env bash
# t_d50c9503 smoke: real SDL window path — send F5 / F9 with xdotool under Xvfb
# and check the state file + the guest continuing to render.
set -u
WT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$WT/target/dq8-rfix5
CODE=${DQ8_CODE:-/home/hermes/DQ8/DQ8.code}
ROMFS=${DQ8_ROMFS:-/home/hermes/DQ8/target/dq8.romfs}
OUT=$WT/target/ss-smoke
mkdir -p "$OUT/userdata" "$OUT/frames"
DISP=:97
rm -f "$WT/target/dq8-state.sav"

pkill -f "Xvfb $DISP" 2>/dev/null
Xvfb $DISP -screen 0 1024x1024x24 >/dev/null 2>&1 &
XVFB=$!
sleep 1
env DISPLAY=$DISP SDL_VIDEODRIVER=x11 DQ8_RUN_GUEST_THREADS=1 DQ8_GL=1 \
    DQ8_TRACE_SAVESTATE=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT/frames" \
    DQ8_USER_DATA_DIR="$OUT/userdata" \
    "$BIN" "$CODE" "$ROMFS" > "$OUT/run3.log" 2>&1 &
GAME=$!
sleep 25
WID=$(DISPLAY=$DISP xdotool search --sync --onlyvisible --name "DQ8 static recomp" | head -1)
echo "window id: ${WID:-<none>}"
DISPLAY=$DISP xdotool windowactivate --sync "$WID" 2>/dev/null || true
sleep 1
echo "--- F5 (save) ---"
DISPLAY=$DISP xdotool key --window "$WID" F5
sleep 8
echo "--- F9 (load) ---"
DISPLAY=$DISP xdotool key --window "$WID" F9
sleep 10
kill -9 $GAME 2>/dev/null
kill -9 $XVFB 2>/dev/null
echo "--- result ---"
ls -la "$WT/target/dq8-state.sav" 2>/dev/null || echo "NO STATE FILE"
grep -n "SAVESTATE\|host display" "$OUT/run3.log" | head -20
echo "--- draws after load ---"
grep -c "GL draw" "$OUT/run3.log"
