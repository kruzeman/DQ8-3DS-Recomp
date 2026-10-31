#!/usr/bin/env bash
set -u
cd /home/hermes/DQ8
rm -rf target/userdata
: > /tmp/dq8-sdl2.log
mkdir -p /tmp/dq8_frames
xvfb-run -a -s "-screen 0 1280x1024x32" bash -c '
  export DQ8_DISPLAY=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_GPU=1 DQ8_TRACE_DRAW_FROM=0
  ./target/dq8-startup-sdl DQ8.code target/dq8.romfs > /tmp/dq8-sdl2.log 2>&1 &
  GP=$!
  for i in $(seq 1 30); do
    sleep 2
    ffmpeg -y -f x11grab -video_size 800x480 -i $DISPLAY -vframes 1 /tmp/dq8_frames/f_$i.png 2>>/tmp/dq8-sdl2.log
    kill -0 $GP 2>/dev/null || break
  done
  kill $GP 2>/dev/null
  wait $GP 2>/dev/null
'
echo "=== SDL init ==="; grep -iE "host display|SDL_" /tmp/dq8-sdl2.log | head
echo "frames:"; ls /tmp/dq8_frames/f_*.png 2>/dev/null | tr '\n' ' '; echo
