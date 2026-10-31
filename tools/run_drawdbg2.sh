#!/usr/bin/env bash
export DQ8_CAPTURE_PPM=1   # PPM screen snapshots are opt-in since audit 5.4
set -u
cd /home/hermes/DQ8
rm -rf /tmp/dq8_frames3 target/userdata; mkdir -p /tmp/dq8_frames3
: > /tmp/drawdbg2.log
xvfb-run -a -s "-screen 0 1024x768x32 -ac -nolisten tcp" bash -c '
  export DQ8_DISPLAY=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_GPU=1 DQ8_TRACE_DRAW_FROM=850
  ./target/dq8-startup-sdl DQ8.code target/dq8.romfs > /tmp/drawdbg2.log 2>&1 &
  GP=$!
  for i in $(seq 1 27); do sleep 2; cp /tmp/dq8-screen.ppm /tmp/dq8_frames3/g_$i.ppm 2>/dev/null||true; kill -0 $GP 2>/dev/null||break; done
  kill $GP 2>/dev/null; wait $GP 2>/dev/null
'
echo "===DRAWDBG count==="; grep -c DRAWDBG /tmp/drawdbg2.log
echo "===distinct wh (width x height from 0x82)==="; grep -oE "tex=[0-9A-F]+/[0-9A-F]+/[0-9A-F]+/[0-9A-F]+" /tmp/drawdbg2.log | sort | uniq -c | sort -rn | head -30
echo "===large tex (w!=8)==="; grep DRAWDBG /tmp/drawdbg2.log | awk -F'tex=' '{n=split($2,a,"/");w=strtonum("0x"substr(a[2],1,4));h=strtonum("0x"a[2]);if(w>8||(strtonum("0x"a[2])&0x7FF)>8)print}' | head -10
