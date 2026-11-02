#!/usr/bin/env bash
# t_6b81015d: grab Azahar ground truth of the adventure-log menu from the
# operator's save state (000400000018F100.01.cst -> states slot 1, F4).
set -u
AZ=/tmp/azahar.AppImage
ROM="/home/hermes/DQ8/Dragon Quest VIII - Journey of the Cursed King (USA).3ds"
STATE_SRC=/home/hermes/DQ8/azahar-saves/000400000018F100.01.cst
SDIR=$HOME/.local/share/azahar-emu/states
OUT=/tmp/t6/az
mkdir -p "$SDIR" "$OUT"
cp -f "$STATE_SRC" "$SDIR/000400000018F100.01.cst"
ls -la "$SDIR"

export DISPLAY=:99
pgrep -f "Xvfb :99" >/dev/null || (Xvfb :99 -screen 0 1280x800x24 >/tmp/t6/xvfb.log 2>&1 &)
sleep 2

"$AZ" "$ROM" > "$OUT/azahar.log" 2>&1 &
AZPID=$!
echo "azahar pid=$AZPID"
sleep 35
xdotool search --name "Azahar" | head -5
xdotool search --name "Azahar" | head -1 | xargs -r xdotool windowactivate --sync 2>/dev/null
sleep 1
xdotool key --window "$(xdotool search --name 'Azahar' | head -1)" F4 2>/dev/null || xdotool key F4
sleep 6
ffmpeg -v error -f x11grab -video_size 1280x800 -i :99 -frames:v 1 "$OUT/shot_after_load.png" -y
echo "shot written"
ls -la "$OUT"
