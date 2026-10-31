#!/usr/bin/env bash
# Fixed package build: test/pkg2 (operator package lineage, round 9/10 escapes)
# + cherry-picked Block-IR fix 008def8 (= 8b58d77) + fixture bfea223, with the
# SAME 24-region S1c emission the operator package shipped with.
set -euo pipefail
cd /home/hermes/.hermes/kanban/workspaces/t_1dd14349/dq8

echo "== cargo build --release (dq8tool) $(date -Is) =="
cargo build --release

export DQ8_S1_CFG="0x001A93A8,0x001A9428,0x001A96C8,0x001A9904,0x001A99A8,0x00194D64,0x00195D44,0x001A72D4,0x001A78C0,0x001A7BCC,0x001A9D70,0x001AA05C,0x001AA1A0,0x001AA3B0,0x001AA4F4,0x001AA6F4,0x001AA828,0x001AA9B8,0x001A982C,0x001A9A88,0x00196A78,0x00193A78,0x001A6EA4,0x001A7068"
export DQ8_S1_CFG_END="0x001A9410,0x001A9608,0x001A97B8,0x001A9990,0x001A9A64,0x001956B8,0x00196A40,0x001A78AC,0x001A7BB4,0x001A7E38,0x001AA054,0x001AA194,0x001AA3A8,0x001AA4E8,0x001AA6EC,0x001AA81C,0x001AA9B4,0x001AB9BC,0x001A98DC,0x001A9AD0,0x00197394,0x0019405C,0x001A6F14,0x001A712C"
export BUILD_JOBS=6
export DQ8_GL=1

echo "== build_split.sh target/dq8-s1-fixed $(date -Is) =="
tools/build_split.sh target/dq8-s1-fixed
echo "== BUILD DONE $(date -Is) =="
ls -la target/dq8-s1-fixed
