#!/usr/bin/env bash
# t_6edf5d17: build one S2 stage binary from the SAME generated single-TU C as
# the accepted chain build.  The guest C is generated once (target/dq8-startup.c)
# and each variant gets its own SPLIT_DIR so the per-TU object cache never
# thrashes; dq8_main.c (the native runtime TU) is identical across variants,
# which is what makes the A/B honest.
#
# usage: tools/s2_build.sh <out-binary> <split-dir> <DQ8_S2 value|chain>
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
OUT=${1:?out binary}
SPLIT=${2:?split dir}
MODE=${3:-chain}
export DQ8_CODE=/home/hermes/DQ8/DQ8.code
export DQ8_OVERRIDES=tools/overrides/registry.json
export DQ8_GL=1
export DQ8_S1_CFG=0x001A93A8,0x001A9428,0x001A96C8,0x001A9904,0x001A99A8,0x00194D64,0x00195D44,0x001A982C,0x001A9A88,0x00193A78,0x001A7438,0x001A7468,0x001A747C,0x001AA9B8,0x001A9E20
export DQ8_S1_CFG_END=0x001A9410,0x001A9608,0x001A97B8,0x001A9990,0x001A9A64,0x001956B8,0x00196A40,0x001A98DC,0x001A9AD0,0x0019405C,0x001A7468,0x001A747C,0x001A74B4,0x001AA9D0,0x001A9E38
export SPLIT_DIR=$SPLIT
export DQ8_SPLIT_CHAIN=1
export DQ8_SPLIT_SKIP_REGEN=1          # same target/dq8-startup.c for every arm
if [[ $MODE == chain ]]; then
  unset DQ8_S2
else
  export DQ8_S2=$MODE
fi
export BUILD_JOBS=${BUILD_JOBS:-2}
echo "== S2 stage build $(date -Is) mode=$MODE split=$SPLIT out=$OUT =="
bash tools/build_split.sh "$OUT"
echo "== S2 STAGE BUILD DONE $(date -Is) =="
