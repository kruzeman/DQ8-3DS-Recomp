#!/usr/bin/env bash
# t_0b95f1a7 windowed-present A/B: run the same in-world scene flow with the
# SDL window presented (DQ8_DISPLAY=1) under Xvfb, grabbing the actual window
# with ffmpeg x11grab while DQ8_CAPTURE_PPM captures the shadow. If the window
# grabs show black where the PPM snaps show the world, the bug is in the
# present path (renderer-owned); if both agree, it is upstream (guest render).
#
# usage: BWW_PREFIX=bww-gl1 BWW_GL=1 BWW_DUR=420 bash tools/run_blackworld_window.sh
set -u
WT=${BWW_WT:-$(cd "$(dirname "$0")/.." && pwd)}
BIN=${BWW_BIN:-/home/hermes/dq8-pkg10/dq8}
GL=${BWW_GL:-1}
UD_SRC=${BWW_UD_SRC:-/home/hermes/DQ8/target/adventure-log-evidence/honeySave-run3.bin}
DUR=${BWW_DUR:-420}
PREFIX=${BWW_PREFIX:-bww-probe}
SNAP=${BWW_SNAP:-5}
CODE=${BWW_CODE:-/home/hermes/DQ8/DQ8.code}
ROMFS=${BWW_ROMFS:-/home/hermes/DQ8/target/dq8.romfs}
FFMPEG=${BWW_FFMPEG:-/home/hermes/.hermes/tools/ffmpeg-9.0.1-linux-x64/bin/ffmpeg}
SPECS=(30 60 90 195 203 207 211:START 216:START 221 226 231:START 236 241 246 250 256 262 268 274 280 286 292 300 310 320 330 340 360 380)
BUTTON_A=0x1; BUTTON_START=0x8

OUT=$WT/target/blackworld-evidence/$PREFIX
mkdir -p "$OUT/frames" "$OUT/window"
rm -rf "$OUT/userdata"; mkdir -p "$OUT/userdata"
if [ -n "$UD_SRC" ]; then
  mkdir -p "$OUT/userdata/archive-00000004-00000000"
  cp -f "$UD_SRC" "$OUT/userdata/archive-00000004-00000000/honeySave.bin"
fi
printf '0\n' > "$OUT/input.txt"
: > "$OUT/run.log"; : > "$OUT/timeline.log"
EXTRA_ENV=()
[[ -n "${BWW_EXTRA:-}" ]] && read -ra EXTRA_ENV <<< "$BWW_EXTRA"
GLARG=0; [ "$GL" = 1 ] && GLARG=1

export DQ8_HEADLESS=0 DQ8_DISPLAY=1 DQ8_RUN_GUEST_THREADS=1 DQ8_TRACE_INPUT=1
export SDL_VIDEODRIVER=x11
export DQ8_PROGRESS_SECONDS=10 DQ8_CAPTURE_PPM=1 DQ8_CAPTURE_PPM_DIR="$OUT/frames"
export DQ8_FRAME_STATS=1 DQ8_GL=$GLARG
export DQ8_USER_DATA_DIR="$OUT/userdata" DQ8_INPUT_FILE="$OUT/input.txt"

echo "t0=$(date +%s.%N) bin=$BIN gl=$GLARG windowed dur=$DUR extra=${BWW_EXTRA:-}" >> "$OUT/timeline.log"
xvfb-run -a -s "-screen 0 1280x1024x24 -ac" bash -c "
  env ${EXTRA_ENV[*]+"${EXTRA_ENV[*]}"} '$BIN' '$CODE' '$ROMFS' > '$OUT/run.log' 2>&1 &
  gp=\$!
  for ((t=1; t<=$DUR; t++)); do
    sleep 1
    kill -0 \$gp 2>/dev/null || { echo \"t=\$t game exited\" >> '$OUT/timeline.log'; break; }
    if (( t % $SNAP == 0 )); then
      '$FFMPEG' -y -loglevel error -f x11grab -video_size 800x480 -i \$DISPLAY -vframes 1 '$OUT/window/t'\$(printf '%04d' \$t).png 2>> '$OUT/window/grab.log'
      [ -f '$OUT/frames/dq8-screen.ppm' ] &&
        cp -f '$OUT/frames/dq8-screen.ppm' '$OUT/frames/t'\$(printf '%04d' \$t).ppm 2>/dev/null
    fi
    for spec in ${SPECS[*]}; do
      btn=A; base=\$spec
      case \"\$spec\" in *:*) btn=\${spec##*:}; base=\${spec%%:*} ;; esac
      case \"\$btn\" in START|start) value=$BUTTON_START ;; *) value=$BUTTON_A ;; esac
      case \"\$base\" in *-*) d=\${base%%-*}; u=\${base##*-} ;; *) d=\$base; u=\$((base+1)) ;; esac
      if (( t == d )); then printf '%s\n' \"\$value\" > '$OUT/input.txt'; fi
      if (( t == u )); then printf '0\n' > '$OUT/input.txt'; fi
    done
  done
  kill -0 \$gp 2>/dev/null && kill -KILL \$gp 2>/dev/null
  wait \$gp 2>/dev/null
  echo \"exit=\$? t_end=\$(date +%s.%N)\" >> '$OUT/timeline.log'
"
echo "=== $PREFIX (windowed) ==="
echo -n "window grabs: "; ls "$OUT/window"/t*.png 2>/dev/null | wc -l
echo -n "ppm snaps: "; ls "$OUT/frames"/t*.ppm 2>/dev/null | wc -l
grep -E "host display" "$OUT/run.log" | head -4
grep "FRAMESTAT" "$OUT/run.log" | tail -3
tail -2 "$OUT/timeline.log"
