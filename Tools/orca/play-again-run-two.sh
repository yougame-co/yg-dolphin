#!/bin/bash
# play-again-run-two.sh <out> <command...>: one two-Orca run of Tools/orca/queue-e2e.mjs (from this
# checkout), never while the owner's own Orca is open. It waits OUTSIDE the locks (every 30 s, up to
# WAIT_MAX seconds, default 90 min) until that Orca is closed and the machine has room (1-minute load
# under 20, memory free over 20%), then takes orca-run-solo.lock and orca-run.lock (as the other
# workflows do) without waiting long, checks again inside, and runs. Exit 9: gave up waiting.
OUT=$1
shift
LOCKS=$HOME/Documents/brawl-dolphin
RUN=${PLAY_AGAIN_RUN:-$HOME/ClaudeWorktrees/orca-play-again-run}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$RUN/tmp"
playing() { ps -axo comm= | grep -qE 'Resources/runtimes/orca/Orca.app/Contents/MacOS/Orca|yougame-desktop/orca/apps/'; }
roomy() {
  local load free
  load=$(sysctl -n vm.loadavg | awk '{print int($2)}')
  free=$(memory_pressure | awk '/free percentage/ {gsub("%","",$5); print int($5)}')
  [ "${load:-99}" -lt 20 ] && [ "${free:-0}" -gt 20 ]
}
export -f playing
deadline=$(($(date +%s) + ${WAIT_MAX:-5400}))
: >"$OUT.wait"
while :; do
  while playing || ! roomy; do
    if [ "$(date +%s)" -gt "$deadline" ]; then
      echo "gave up waiting $(date +%T)" >>"$OUT.wait"
      exit 9
    fi
    echo "waiting $(date +%T): playing=$(playing && echo yes || echo no) load $(sysctl -n vm.loadavg)" >>"$OUT.wait"
    sleep 30
  done
  # Both locks, or back to waiting (another agent's run holds them): never hold one while waiting.
  OUT="$OUT" RUN="$RUN" REPO="$REPO" flock -w 20 "$LOCKS/orca-run-solo.lock" flock -w 20 "$LOCKS/orca-run.lock" bash -c '
    if playing; then exit 10; fi
    echo "start $(date +%T) load $(sysctl -n vm.loadavg)" >"$OUT"
    cd "$REPO" && TMPDIR=$RUN/tmp "$@" >>"$OUT" 2>&1
    rc=$?
    echo "exit $rc $(date +%T)" >>"$OUT"
    exit 0
  ' _ "$@"
  rc=$?
  if [ $rc -eq 0 ]; then
    exit 0
  fi
  echo "not run ($rc) $(date +%T)" >>"$OUT.wait"
  sleep 30
done
