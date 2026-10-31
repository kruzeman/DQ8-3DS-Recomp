#!/usr/bin/env bash
export DQ8_CAPTURE_PPM=1   # PPM screen snapshots are opt-in since audit 5.4
set -u
cd /home/hermes/DQ8
rm -rf target/userdata target/userdata_save; mkdir -p /tmp/dq8_frames2
: > /tmp/drawdbg.log
xvfb-run -a -s "-screen 0 1024x768x32 -ac -nolisten tcp" bash -c '
  export DQ8_DISPLAY=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_GPU=1 DQ8_TRACE_DRAW_FROM=1080
  ./target/dq8-startup-sdl DQ8.code target/dq8.romfs > /tmp/drawdbg.log 2>&1 &
  GP=$!
  for i in $(seq 1 27); do sleep 2; cp /tmp/dq8-screen.ppm /tmp/dq8_frames2/f_$i.ppm 2>/dev/null||true; kill -0 $GP 2>/dev/null||break; done
  kill $GP 2>/dev/null; wait $GP 2>/dev/null
'
echo "===draws captured (DRAWDBG)==="; grep -c DRAWDBG /tmp/drawdbg.log
echo "===tex0 states==="; grep DRAWDBG /tmp/drawdbg.log | sed -E 's/.*tex=([0-9A-F]+)\/([0-9A-F]+)\/([0-9A-F]+)\/([0-9A-F]+).*/tex cfg=\1 wh=\2 addr=\3 fmt=\4/' | sort | uniq -c | sort -rn | head
echo "===non-zero tex draws==="; grep -cE "tex=[1-9]" /tmp/drawdbg.log
echo "===quit?==="; grep -c quit /tmp/drawdbg.log
python3 /home/hermes/.hermes/profiles/coder/cache/scratch/anim2b.py /tmp/dq8_frames2 f_