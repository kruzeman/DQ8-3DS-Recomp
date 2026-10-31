#!/usr/bin/env bash
# t_3de68379 stage builds: same recipe as tools/build_texcache3.sh (the
# operator-accepted base: DQ8_GL=1, overrides registry, the 15-region S1c
# gate list) with an isolated SPLIT_DIR so the per-TU object cache of the
# per-instruction build and the chain build never thrash each other.
#
# usage: tools/build_3ds_stage.sh <out-binary> <split-dir> [chain=0|1]
#
# chain=1 sets DQ8_SPLIT_CHAIN=1 (tools/split_dispatch.py emits the
# 3dsrecomp-style label/goto body).  Chain parts are ~3x larger than the
# per-instruction parts (one label + prologue per translated PC), so the
# clang pool is capped at 2 jobs: one chain TU peaks near 1.5 GiB RSS
# (measured on dq8_part_011: 22.3 s / 1 557 108 KiB vs 9.2 s / 555 136 KiB).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

OUT=${1:?out binary}
SPLIT=${2:?split dir}
CHAIN=${3:-0}

export DQ8_CODE=/home/hermes/DQ8/DQ8.code
export DQ8_OVERRIDES=tools/overrides/registry.json
export DQ8_GL=1
export DQ8_S1_CFG=0x001A93A8,0x001A9428,0x001A96C8,0x001A9904,0x001A99A8,0x00194D64,0x00195D44,0x001A982C,0x001A9A88,0x00193A78,0x001A7438,0x001A7468,0x001A747C,0x001AA9B8,0x001A9E20
export DQ8_S1_CFG_END=0x001A9410,0x001A9608,0x001A97B8,0x001A9990,0x001A9A64,0x001956B8,0x00196A40,0x001A98DC,0x001A9AD0,0x0019405C,0x001A7468,0x001A747C,0x001A74B4,0x001AA9D0,0x001A9E38
export SPLIT_DIR=$SPLIT
if [[ $CHAIN == 1 ]]; then
  export DQ8_SPLIT_CHAIN=1
  export BUILD_JOBS=${BUILD_JOBS:-2}
  echo "== stage build (CHAIN) $(date -Is) =="
else
  export BUILD_JOBS=${BUILD_JOBS:-16}
  echo "== stage build (per-instruction) $(date -Is) =="
fi
bash tools/build_split.sh "$OUT"
echo "== STAGE BUILD DONE $(date -Is) =="
ls -la "$OUT"
