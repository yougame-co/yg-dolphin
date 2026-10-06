#!/bin/bash
# play-again-run-two.sh <out> <command...>: one two-Orca run of Tools/orca/queue-e2e.mjs (from this
# checkout) as a check in the Mac's run queue (Tools/orca/run-lock.py): never while the owner's own
# Orca is open, up to three checks at once while the machine has room, never beside a perf run, first
# come first served. It waits up to WAIT_MAX seconds (default 90 min) for its turn, writing what it
# waits for to <out>.wait. <out> gets "start ...", the command's output, then "exit <code> <time>".
# Exit 9: gave up waiting. Exit 10: stopped because the owner opened his Orca (no "exit" line).
OUT=$1
shift
case $OUT in /*) ;; *) OUT=$PWD/$OUT ;; esac
RUN=${PLAY_AGAIN_RUN:-$HOME/ClaudeWorktrees/orca-play-again-run}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$RUN/tmp"
: >"$OUT.wait"
cd "$REPO" || exit 2
OUT="$OUT" RUN="$RUN" python3 Tools/orca/run-lock.py check --label "play-again $(basename "$OUT")" \
  --wait-max "${WAIT_MAX:-5400}" -- bash -c '
    echo "start $(date +%T) load $(sysctl -n vm.loadavg)" >"$OUT"
    TMPDIR=$RUN/tmp "$@" >>"$OUT" 2>&1
    echo "exit $? $(date +%T)" >>"$OUT"
  ' _ "$@" 2>>"$OUT.wait"
rc=$?
[ $rc -eq 9 ] && echo "gave up waiting $(date +%T)" >>"$OUT.wait"
[ $rc -eq 10 ] && echo "stopped: the owner's Orca opened $(date +%T)" >>"$OUT.wait"
exit $rc
