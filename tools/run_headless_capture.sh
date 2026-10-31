#!/usr/bin/env bash
export DQ8_CAPTURE_PPM=1   # PPM screen snapshots are opt-in since audit 5.4
set -u
cd /home/hermes/DQ8
clang -std=c11 -O0 -g0 -w target/dq8-startup.c -o target/dq8-startup-headless -lm 2>&1 | tail -3
echo "BUILD_EXIT=${PIPESTATUS[0]}"
rm -rf target/userdata; mkdir -p /tmp/h_frames
: > /tmp/h-run.log
DQ8_HEADLESS=1 DQ8_DISPLAY=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_FS=1 DQ8_TRACE_GPU=1 DQ8_RUN_GUEST_THREADS=1 \
  timeout 100 ./target/dq8-startup-headless DQ8.code target/dq8.romfs > /tmp/h-run.log 2>&1 &
GP=$!
for i in $(seq 1 45); do sleep 2; cp /tmp/dq8-screen.ppm /tmp/h_frames/h_$i.ppm 2>/dev/null||true; done
wait $GP 2>/dev/null
echo "===FS save sequence?==="; grep -cE "080C00C2|ubll|080201C2|native FS path" /tmp/h-run.log
grep -E "ubll|native FS path|080201C2" /tmp/h-run.log | head
echo "===max draw==="; grep -oE "count=[0-9]+" /tmp/h-run.log | sort -t= -k2 -n | uniq | tail -1
python3 /home/hermes/.hermes/profiles/coder/cache/scratch/anim2b.py /tmp/h_frames h_