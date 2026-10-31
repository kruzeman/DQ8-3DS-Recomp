#!/usr/bin/env bash
set -u
cd /home/hermes/DQ8
rm -rf target/userdata
timeout 90 env DQ8_TRACE_DRAW_FROM=3450 ./target/dq8-startup-headless DQ8.code target/dq8.romfs > /tmp/drawdbg3.log 2>&1 &
GP=$!
sleep 88
kill $GP 2>/dev/null; wait $GP 2>/dev/null
echo "===DRAWDBG count==="; grep -c DRAWDBG /tmp/drawdbg3.log
echo "===tex states in dialog draws==="; grep DRAWDBG /tmp/drawdbg3.log | sed -E 's/.*tex=([0-9A-F]+)\/([0-9A-F]+)\/([0-9A-F]+)\/([0-9A-F]+).*/cfg=\1 wh=\2 addr=\3 fmt=\4/' | sort | uniq -c | sort -rn
echo "===first 8 DRAWDBG==="; grep DRAWDBG /tmp/drawdbg3.log | head -8
echo "===any large tex (addr high)?==="; grep -oE "tex=[0-9A-F]+/[0-9A-F]+/[0-9A-F]+/[0-9A-F]+" /tmp/drawdbg3.log | sort -u | head -40