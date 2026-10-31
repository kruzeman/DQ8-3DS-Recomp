#!/usr/bin/env bash
# Sequential before/after joint rows for t_5ebeb086 (F4/F5 acceptance).
# Runs one at a time on a quiet machine; each writes tools/joint/<name>/.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.."
J="tools/joint_measure.py"
export DQ8_RUN_GUEST_THREADS=1

run() { # name, extra-args..., env via RUNENV
  local name=$1; shift
  echo "=== $(date +%H:%M:%S) RUN $name ==="
  env $RUNENV DISPLAY=:163 python3 "$J" "$name" --no-trace-time "$@" \
      > "tools/joint-$name.driver.log" 2>&1
  echo "driver rc=$? name=$name"
}

# Windowed dialog (primary): logo window + steady dialog window.
RUNENV=""
run base-dialog  --bin target/dq8-base            --max-wall 165 --window 6 20 --window 100 150
RUNENV="DQ8_SPLIT_RENDER=1"
run split-dialog --bin target/dq8-split-render     --max-wall 165 --window 6 20 --window 100 150

# Headless dialog contrast (F4: guest throughput windowed vs headless).
# DQ8_CAPTURE_PPM=1 gives the shadow a consumer so unique-frame hashes are real.
RUNENV="DQ8_CAPTURE_PPM=1"
run base-headless  --bin target/dq8-base        --headless --max-wall 165 --window 6 20 --window 100 150
RUNENV="DQ8_CAPTURE_PPM=1 DQ8_SPLIT_RENDER=1"
run split-headless --bin target/dq8-split-render --headless --max-wall 165 --window 6 20 --window 100 150

# Movie scenario (audit D): A press at t=115 dismisses dialog, playback window.
RUNENV=""
run base-movie  --bin target/dq8-base        --max-wall 215 --window 6 20 --window 135 205 --press-at 115
RUNENV="DQ8_SPLIT_RENDER=1"
run split-movie --bin target/dq8-split-render --max-wall 215 --window 6 20 --window 135 205 --press-at 115

echo "=== $(date +%H:%M:%S) ALL DONE ==="
