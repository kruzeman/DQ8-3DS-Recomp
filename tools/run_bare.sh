#!/usr/bin/env bash
export DQ8_CAPTURE_PPM=1   # PPM screen snapshots are opt-in since audit 5.4
set -u
cd /home/hermes/DQ8
rm -rf target/userdata
: > /tmp/dq8-bare.log
mkdir -p /tmp/dq8_frames
xvfb-run -a -s "-screen 0 1024x768x32 -ac" bash -c '
  export DQ8_DISPLAY=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_GPU=1
  ./target/dq8-startup-sdl DQ8.code target/dq8.romfs > /tmp/dq8-bare.log 2>&1 &
  GP=$!
  for i in $(seq 1 30); do
    sleep 2
    cp /tmp/dq8-screen.ppm /tmp/dq8_frames/b_$i.ppm 2>/dev/null || true
    kill -0 $GP 2>/dev/null || break
  done
  kill $GP 2>/dev/null; wait $GP 2>/dev/null
'
echo "=== quit? ==="; grep -c "quit requested" /tmp/dq8-bare.log
echo "=== draw max ==="; grep -oE "count=[0-9]+" /tmp/dq8-bare.log | sort -t= -k2 -n | tail -1
echo "=== last 5 ==="; tail -5 /tmp/dq8-bare.log
python3 /home/hermes/.hermes/profiles/coder/cache/scratch/anim2b.py
