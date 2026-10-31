#!/usr/bin/env bash
set -u
cd /home/hermes/DQ8
rm -rf target/userdata
mkdir -p /tmp/dq8_frames
: > /tmp/dq8-run.log
cd /home/hermes/DQ8
xvfb-run -a -s "-screen 0 1024x768x32" bash -c '
  export DQ8_DISPLAY=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_GPU=1 DQ8_TRACE_IPC=1
  ./target/dq8-startup DQ8.code target/dq8.romfs > /tmp/dq8-run.log 2>&1 &
  GP=$!
  for i in $(seq 1 50); do
    sleep 2
    ffmpeg -y -f x11grab -video_size 800x480 -i $DISPLAY -vframes 1 /tmp/dq8_frames/f_$i.png 2>/tmp/ffmpeg_$i.log || true
    kill -0 $GP 2>/dev/null || break
  done
  kill $GP 2>/dev/null
  wait $GP 2>/dev/null
'
echo "=== saved frames ==="
ls /tmp/dq8_frames/
echo "=== run log tail ==="
tail -30 /tmp/dq8-run.log
