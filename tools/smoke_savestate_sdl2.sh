#!/usr/bin/env bash
# t_d50c9503 smoke v2: real SDL window, F5/F9 via xdotool under Xvfb, with the
# periodic guest-progress dump on so "the guest kept running after load" is
# provable (pc advances) instead of inferred.  Also snapshots the presented PPM
# before the save, before the load and after the load.
set -u
WT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$WT/target/dq8-rfix5
CODE=${DQ8_CODE:-/home/hermes/DQ8/DQ8.code}
ROMFS=${DQ8_ROMFS:-/home/hermes/DQ8/target/dq8.romfs}
OUT=$WT/target/ss-smoke2
mkdir -p "$OUT/userdata" "$OUT/frames"
DISP=:97
rm -f "$WT/target/dq8-state.sav"

pkill -f "Xvfb $DISP" 2>/dev/null
Xvfb $DISP -screen 0 1024x1024x24 >/dev/null 2>&1 &
XVFB=$!
sleep 1
env DISPLAY=$DISP SDL_VIDEODRIVER=x11 DQ8_RUN_GUEST_THREADS=1 DQ8_GL=1 \
    DQ8_TRACE_SAVESTATE=1 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT/frames" \
    DQ8_PROGRESS_SECONDS=5 DQ8_USER_DATA_DIR="$OUT/userdata" \
    "$BIN" "$CODE" "$ROMFS" > "$OUT/run.log" 2>&1 &
GAME=$!
sleep 22
WID=$(DISPLAY=$DISP xdotool search --sync --onlyvisible --name "DQ8 static recomp" | head -1)
DISPLAY=$DISP xdotool windowactivate --sync "$WID" 2>/dev/null || true
sleep 1
cp -f "$OUT/frames/dq8-screen.ppm" "$OUT/before-f5.ppm" 2>/dev/null
echo "--- F5 (save) ---"
DISPLAY=$DISP xdotool key --window "$WID" F5
sleep 9
cp -f "$OUT/frames/dq8-screen.ppm" "$OUT/before-f9.ppm" 2>/dev/null
echo "--- F9 (load) ---"
DISPLAY=$DISP xdotool key --window "$WID" F9
sleep 12
cp -f "$OUT/frames/dq8-screen.ppm" "$OUT/after-f9.ppm" 2>/dev/null
kill -9 $GAME 2>/dev/null
kill -9 $XVFB 2>/dev/null
echo "--- savestate lines ---"
grep -n "SAVESTATE" "$OUT/run.log"
echo "--- guest progress (pc must advance after the load) ---"
grep -n "guest snapshot pc=" "$OUT/run.log" | sed -n '1,40p'
echo "--- frame sizes ---"
ls -l "$OUT"/*.ppm 2>/dev/null
