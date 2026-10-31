#!/usr/bin/env bash
# t_41f38df0: build dq8-rfix11 = rfix10 + (a) mkdir-safe CMDBIN dump (bw-cam4
# dumped nothing because the target dir did not exist), (b) DQ8_TRACE_UNIDRAW_MAX
# (the hard 400-line UNICAM budget died before the cutscene at t~436), and
# (c) DQ8_TRACE_VBUF fingerprint (answered: mesh static) + multi-range DQ8_TRACE_WRITE (LINEAR + FCRAM views) + dead-gated RAM scan.
# Same recipe as dq8-rfix7; only the runtime TUs change, so the guest split
# cache reuses and only the runtime TU + link are redone.
set -euo pipefail
cd /home/hermes/DQ8/worktrees/black-world-t_0b95f1a7
export DQ8_CODE=/home/hermes/DQ8/DQ8.code
export DQ8_OVERRIDES=tools/overrides/registry.json
export DQ8_GL=1
export DQ8_S1_CFG=0x001A93A8,0x001A9428,0x001A96C8,0x001A9904,0x001A99A8,0x00194D64,0x00195D44,0x001A982C,0x001A9A88,0x00193A78,0x001A7438,0x001A7468,0x001A747C,0x001AA9B8,0x001A9E20
export DQ8_S1_CFG_END=0x001A9410,0x001A9608,0x001A97B8,0x001A9990,0x001A9A64,0x001956B8,0x00196A40,0x001A98DC,0x001A9AD0,0x0019405C,0x001A7468,0x001A747C,0x001A74B4,0x001AA9D0,0x001A9E38
bash tools/build_split.sh target/dq8-rfix11
