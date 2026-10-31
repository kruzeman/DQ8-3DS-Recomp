#!/usr/bin/env bash
export DQ8_CAPTURE_PPM=1   # PPM screen snapshots are opt-in since audit 5.4
set -u
cd /home/hermes/DQ8
rm -rf target/userdata /tmp/dq8_frames
mkdir -p /tmp/dq8_frames
: > /tmp/dq8-run2.log
DQ8_HEADLESS=1 DQ8_DISPLAY=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_GPU=1 DQ8_TRACE_IPC=1 \
  ./target/dq8-startup DQ8.code target/dq8.romfs > /tmp/dq8-run2.log 2>&1 &
GP=$!
for i in $(seq 1 20); do
  sleep 3
  cp /tmp/dq8-screen.ppm /tmp/dq8_frames/ppm_$i.ppm 2>/dev/null || true
  kill -0 $GP 2>/dev/null || break
done
kill $GP 2>/dev/null
wait $GP 2>/dev/null
echo "done; frames:" $(ls /tmp/dq8_frames/ | wc -l)
