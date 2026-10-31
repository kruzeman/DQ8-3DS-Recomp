#!/usr/bin/env bash
# Movie decode-throughput matrix for t_5c5830df (levers: guest -O2 TUs,
# extended S1 coverage). Rows on the SAME seeded 40M-main-step segment via
# tools/ab_movie_decode.sh (wall + y2r conversions + aggregate M-instr/s).
#
# Gates (runtime DQ8_S1_CFG): G10 = the F3 10-region list; all = 15 regions
# (10 + the 5 t_5c5830df additions). All rows default DQ8_DISABLE_OVERRIDES=1
# except the explicit lever-3 probe pair.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.."
SCRATCH=${SCRATCH:-/home/hermes/.hermes/profiles/renderer/cache/scratch}
G10=0x001A93A8,0x001A9428,0x001A96C8,0x001A9904,0x001A99A8,0x00194D64,0x00195D44,0x001A982C,0x001A9A88,0x00193A78
G9=0x001A9428,0x001A96C8,0x001A9904,0x001A99A8,0x00194D64,0x00195D44,0x001A982C,0x001A9A88,0x00193A78
O0=target/dq8-movie-o0-ext
O2=target/dq8-movie-o2-ext

run() { # run <tag> <bin> <gate> <extra-env>
  local tag=$1 bin=$2 gate=$3 extra=${4:-}
  if [ -n "$extra" ]; then
    AB_BIN=$bin AB_TAG=$tag AB_S1="$gate" AB_ENV="DQ8_DISABLE_OVERRIDES=1 $extra" tools/ab_movie_decode.sh
  else
    AB_BIN=$bin AB_TAG=$tag AB_S1="$gate" AB_ENV="DQ8_DISABLE_OVERRIDES=1" tools/ab_movie_decode.sh
  fi
}

# Interleaved rounds so host-load drift hits every config equally.
for rep in 1 2 3; do
  run "A-o0-g10-r$rep" $O0 "$G10"
  run "C-o2-g10-r$rep" $O2 "$G10"
  run "B-o0-g15-r$rep" $O0 all
  run "D-o2-g15-r$rep" $O2 all
done

# Lever-3 probe (the only config the card says could pay): -O0, S1 gate minus
# region 0x001A93A8 (the packed_field_unpack_loop window), override ON vs OFF.
for rep in 1 2; do
  AB_BIN=$O0 AB_TAG="E-o0-g9-ovON-r$rep" AB_S1="$G9" AB_ENV="" tools/ab_movie_decode.sh
  AB_BIN=$O0 AB_TAG="E-o0-g9-ovOFF-r$rep" AB_S1="$G9" AB_ENV="DQ8_DISABLE_OVERRIDES=1" tools/ab_movie_decode.sh
done
echo "MATRIX DONE"
