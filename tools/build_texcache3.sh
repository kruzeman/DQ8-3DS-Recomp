#!/usr/bin/env bash
# t_66c89cf2 round 1: build the persistent-texture-cache binary IN THIS WORKTREE
# (tools/build_texcache2.sh hardcodes cd to the savestate worktree — the
# hotspot another worker is editing; never use it from here).
# Same recipe as tools/build_texcache2.sh (15-region S1c + DQ8_GL=1).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
export DQ8_CODE=/home/hermes/DQ8/DQ8.code
export DQ8_OVERRIDES=tools/overrides/registry.json
export DQ8_GL=1
export DQ8_S1_CFG=0x001A93A8,0x001A9428,0x001A96C8,0x001A9904,0x001A99A8,0x00194D64,0x00195D44,0x001A982C,0x001A9A88,0x00193A78,0x001A7438,0x001A7468,0x001A747C,0x001AA9B8,0x001A9E20
export DQ8_S1_CFG_END=0x001A9410,0x001A9608,0x001A97B8,0x001A9990,0x001A9A64,0x001956B8,0x00196A40,0x001A98DC,0x001A9AD0,0x0019405C,0x001A7468,0x001A747C,0x001A74B4,0x001AA9D0,0x001A9E38
export BUILD_JOBS=${BUILD_JOBS:-16}
echo "== build_texcache3 $(date -Is) =="
bash tools/build_split.sh "${1:-target/dq8-texcache3}"
echo "== BUILD DONE $(date -Is) =="
ls -la "${1:-target/dq8-texcache3}"
