#!/usr/bin/env bash
#
# tools/lock_run.sh -- host-wide advisory mutex for DQ8 builds and timed runs.
#
# Several cards build the same 35-37 guest translation units and then measure
# frame times on the same 24-vCPU host.  A second build, or any timed run
# started while a build is running, makes both parties' numbers meaningless.
# This wrapper serialises them through one host-wide flock(2) file so parallel
# cards queue instead of thrashing.
#
# usage:
#   tools/lock_run.sh [--kind build|measure] [--no-wait] -- <command...>
#
#     --kind build     (default) a build: compiles/links.  Waits for whatever
#                      holds the lock, including a running measurement.
#     --kind measure   a timed run / benchmark.  Refuses to start while a build
#                      holds the lock (measurements have priority, but they are
#                      never allowed to pollute a build).  If another
#                      measurement is running it waits for it.
#     --no-wait        do not queue: print who holds the lock and exit 75.
#
# Lock file: ${DQ8_LOCK_DIR:-/home/hermes/DQ8/.locks}/build.lock
# The lock directory defaults to the host-wide path on purpose: it must be the
# SAME file for every worktree and every card, otherwise it does not coordinate
# anything.  DQ8_LOCK_DIR exists for tests and for a second host.
#
# Holder record: ${DQ8_LOCK_DIR}/holder -- kind, pid, start time (epoch + ISO),
# cwd and the command line of the current holder.  Waiters print it, so a human
# can tell a real 50-minute -O1 build from a hung one.
#
# Stale locks: flock(2) is owned by the kernel and is released when the holding
# process dies (including SIGKILL / OOM), so there is nothing to clean up after
# a crash -- the lock file itself may stay on disk and is harmless.  The holder
# record is only advisory decoration; a dead holder cannot keep the lock.
#
# Reentrancy: the child environment inherits DQ8_LOCK_HELD.  A wrapper started
# inside an already-locked command (e.g. tools/build_locked.sh invoked through
# tools/lock_run.sh) therefore runs directly instead of deadlocking on itself.
#
set -uo pipefail

PROG=lock_run
DEFAULT_LOCK_DIR=/home/hermes/DQ8/.locks
LOCK_DIR=${DQ8_LOCK_DIR:-$DEFAULT_LOCK_DIR}
LOCK_FILE=$LOCK_DIR/build.lock
HOLDER=$LOCK_DIR/holder
KIND=build
NOWAIT=0
EVERY=30            # seconds between "who holds the lock" reports
EX_TEMPFAIL=75      # cannot acquire now (same meaning as sysexits.h)

usage() {
  sed -n '8,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
}

die() { printf '[%s] error: %s\n' "$PROG" "$*" >&2; exit 2; }

while (( $# )); do
  case $1 in
    --kind)
      [[ $# -ge 2 ]] || die "--kind needs build|measure"
      KIND=$2; shift 2 ;;
    --kind=*) KIND=${1#*=}; shift ;;
    --no-wait|--nowait) NOWAIT=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; break ;;
    -*) die "unknown option '$1' (see --help)" ;;
    *) break ;;
  esac
done
case $KIND in build|measure) ;; *) die "--kind must be 'build' or 'measure', got '$KIND'" ;; esac
(( $# )) || die "no command given: tools/lock_run.sh [--kind build|measure] [--no-wait] -- <command...>"

# ---- nested run: the lock is already held by this process tree -------------
if [[ -n ${DQ8_LOCK_HELD:-} ]]; then
  printf '[%s] nested %s run (outer hold: %s) -- lock already held here, running directly\n' \
    "$PROG" "$KIND" "$DQ8_LOCK_HELD" >&2
  exec "$@"
fi

# ---- holder record ---------------------------------------------------------
# Sourced in a subshell only: the file is written by us, values are %q-quoted.
read_holder() {
  H_KIND='' H_PID='' H_EPOCH='' H_ISO='' H_CWD='' H_CMD=''
  [[ -r $HOLDER ]] || return 1
  local out
  out=$( ( set -u; . "$HOLDER" >/dev/null 2>&1; \
           printf '%s\n%s\n%s\n%s\n%s\n%s\n' \
             "$kind" "$pid" "$start_epoch" "$start_iso" "$cwd" "$cmd" ) ) || return 1
  local -a f=()
  mapfile -t f <<< "$out"
  (( ${#f[@]} >= 6 )) || return 1
  H_KIND=${f[0]} H_PID=${f[1]} H_EPOCH=${f[2]} H_ISO=${f[3]} H_CWD=${f[4]} H_CMD=${f[5]}
  return 0
}

# report_holder <prefix> -- says who holds the lock right now (to stderr)
report_holder() {
  local prefix=$1 now el
  now=$(date +%s)
  if read_holder; then
    el=$(( now - H_EPOCH ))
    (( el < 0 )) && el=0
    printf '[%s] %s: lock is held by pid %s (%s) for %ss\n' \
      "$PROG" "$prefix" "$H_PID" "$H_KIND" "$el" >&2
    printf '[%s]   started %s, cwd=%s\n' "$PROG" "$H_ISO" "$H_CWD" >&2
    printf '[%s]   cmd: %s\n' "$PROG" "$H_CMD" >&2
    printf '[%s]   lock: %s\n' "$PROG" "$LOCK_FILE" >&2
    kill -0 "$H_PID" 2>/dev/null || \
      printf '[%s]   note: pid %s is gone; flock is released on process death, so this record is stale\n' \
        "$PROG" "$H_PID" >&2
  else
    printf '[%s] %s: %s is held but has no holder record (a plain flock user, or the holder died before writing it)\n' \
      "$PROG" "$prefix" "$LOCK_FILE" >&2
  fi
}

# write_holder <kind> <pid> <epoch> <iso> <cwd> <cmd>
write_holder() {
  local tmp="$HOLDER.tmp.$$"
  {
    printf 'kind=%q\n' "$1"
    printf 'pid=%q\n' "$2"
    printf 'start_epoch=%q\n' "$3"
    printf 'start_iso=%q\n' "$4"
    printf 'cwd=%q\n' "$5"
    printf 'cmd=%q\n' "$6"
  } > "$tmp" 2>/dev/null && mv -f "$tmp" "$HOLDER" 2>/dev/null || rm -f "$tmp" 2>/dev/null
}

# ---- acquire ---------------------------------------------------------------
mkdir -p "$LOCK_DIR" 2>/dev/null || die "cannot create lock dir '$LOCK_DIR'"
[[ -w $LOCK_DIR ]] || die "lock dir '$LOCK_DIR' is not writable"
exec {LOCKFD}>>"$LOCK_FILE" || die "cannot open lock file '$LOCK_FILE'"

acquired=0
if flock -n "$LOCKFD"; then
  acquired=1
else
  # Busy.  A measurement never queues behind a build: it reports why it
  # refuses so the card can be re-run once the build is done.
  if [[ $KIND == measure ]]; then
    read_holder || true
    if [[ ${H_KIND:-} == build ]]; then
      report_holder "refusing to start a measurement"
      printf '[%s] REFUSED: a --kind build holds %s; rerun this measurement after the build finishes.\n' \
        "$PROG" "$LOCK_FILE" >&2
      exit $EX_TEMPFAIL
    fi
  fi
  if (( NOWAIT )); then
    report_holder "refusing to wait (--no-wait)"
    printf '[%s] REFUSED: lock is busy and --no-wait was given.\n' "$PROG" >&2
    exit $EX_TEMPFAIL
  fi

  read_holder || true
  if [[ ${H_KIND:-} == measure ]]; then
    printf '[%s] waiting: a --kind measure is running; measurements have priority, so this %s queues behind it\n' \
      "$PROG" "$KIND" >&2
  fi
  report_holder "waiting"
  waited=0
  until flock -n "$LOCKFD"; do
    sleep 1
    waited=$(( waited + 1 ))
    if (( waited % EVERY == 0 )); then
      report_holder "still waiting after ${waited}s"
    fi
  done
  printf '[%s] lock became free after %ss\n' "$PROG" "$waited" >&2
  acquired=1
fi
(( acquired )) || die "could not acquire '$LOCK_FILE'"

# ---- hold ----------------------------------------------------------------
MY_EPOCH=$(date +%s)
MY_ISO=$(date -Is)
MY_CWD=$(pwd)
MY_CMD=$(printf '%q ' "$@"); MY_CMD=${MY_CMD% }
write_holder "$KIND" "$$" "$MY_EPOCH" "$MY_ISO" "$MY_CWD" "$MY_CMD"

printf '[%s] acquired %s lock: pid %s, started %s, cmd: %s\n' \
  "$PROG" "$KIND" "$$" "$MY_ISO" "$MY_CMD" >&2

cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  # Only remove the record if it is still ours.
  read_holder || true
  if [[ ${H_PID:-} == "$$" ]]; then rm -f "$HOLDER"; fi
  flock -u "$LOCKFD" 2>/dev/null
  exec {LOCKFD}>&-
  exit $rc
}
trap cleanup EXIT INT TERM

export DQ8_LOCK_HELD="$KIND:$$"
export DQ8_LOCK_DIR="$LOCK_DIR"
export DQ8_LOCK_FILE="$LOCK_FILE"

"$@"
