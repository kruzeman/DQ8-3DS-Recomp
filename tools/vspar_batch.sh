#!/usr/bin/env bash
# vspar_batch.sh -- interleaved A/B driver for the vertex-pool card.
#
#   tools/vspar_batch.sh <scene> <secs> <rounds> 'tag|binary|VAR=val VAR=val' ...
#
# Runs every spec once per round, in the order given, so that round r sees the
# same thermal/load condition for every arm.  Each arm names its own binary
# (old and new builds must be compared in the same run).  A spec may omit the
# env list ("tag|binary") or the binary ("tag||VAR=val", uses $VS_BIN).
set -u
SCENE=${1:?scene}; SECS=${2:-20}; ROUNDS=${3:-1}
shift 3 2>/dev/null || shift $#
for r in $(seq 1 "$ROUNDS"); do
  for spec in "$@"; do
    tag=${spec%%|*}
    rest=${spec#*|}
    bin=${rest%%|*}
    envs=""
    if [ "$rest" != "$bin" ]; then envs=${rest#*|}; fi
    [ -n "$bin" ] || bin=${VS_BIN:?no binary for $tag}
    # shellcheck disable=SC2086
    "$HOME/hotreg/vspar_ab.sh" "$bin" "$SCENE" "$tag-r$r" "$SECS" $envs
  done
done
