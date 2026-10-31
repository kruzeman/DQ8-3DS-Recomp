#!/usr/bin/env bash
set -u
cd /home/hermes/DQ8
rm -rf target/userdata
: > /tmp/dq8-sdl.log
xvfb-run -a -s "-screen 0 1280x1024x32" bash -c '
  export DQ8_DISPLAY=1 DQ8_RUN_GUEST_THREADS=1
  ./target/dq8-startup DQ8.code target/dq8.romfs > /tmp/dq8-sdl.log 2>&1 &
  GP=$!
  sleep 16
  ffmpeg -y -f x11grab -video_size 800x480 -i $DISPLAY -vframes 1 /tmp/dq8_frame_top.png 2>>/tmp/dq8-sdl.log
  ffmpeg -y -f x11grab -video_size 1280x1024 -i $DISPLAY -vframes 1 /tmp/dq8_frame_full.png 2>>/tmp/dq8-sdl.log
  sleep 1
  kill $GP 2>/dev/null
  wait $GP 2>/dev/null
'
echo "=== SDL init lines ==="
grep -iE "host display|SDL_" /tmp/dq8-sdl.log | head
echo "=== frames ==="
ls -la /tmp/dq8_frame_top.png /tmp/dq8_frame_full.png 2>&1
python3 /home/hermes/.hermes/profiles/coder/cache/scratch/framecheck.py 2>/dev/null || true
python3 -c "
import os
from PIL import Image
for p in ['/tmp/dq8_frame_top.png','/tmp/dq8_frame_full.png']:
    if os.path.exists(p):
        im=Image.open(p).convert('L'); px=list(im.getdata())
        print(p, im.size, 'min',min(px),'max',max(px))
    else:
        print(p,'MISSING')
"
