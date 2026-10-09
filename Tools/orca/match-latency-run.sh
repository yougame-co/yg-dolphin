#!/bin/bash
# match-latency-run.sh <check|perf> <tag> <casual|ranked|friends> <brawl|pplus> <matches> [match-latency.mjs args...]
# Two Orcas on this Mac meet on YouGame through match-latency.mjs (ORCA.md, "Match start timing"),
# inside the Mac's run queue (run-lock.py: `perf` has the machine to itself, for timing; `check` may
# share it), SDL HIDAPI off, and a test mark with no added delay (ORCA_TEST_NET_DELAY_MS=0: test
# Orcas only meet each other). Timing runs refuse to start on battery or with under 10 GB free.
#
# Environment (all required unless a default is shown):
#   ML_BIN     the Orca executable (…/Orca.app/Contents/MacOS/Orca)
#   ML_BUILD   the page's word on it, <version>:<commit> (the ticket route lets the current build search)
#   ML_DISC    Brawl (USA) Rev 2;  ML_PPLUS  "Project+ Netplay Launcher.dol" (pplus runs only)
#   ML_SESS    a folder with <name>.cookie for the two /sandbox roster players (match-latency-session.mjs)
#   ML_A, ML_B their names (cy, di);  ML_OUT  where runs go (./match-latency-runs)
#   ML_RUN_LOCK  run-lock.py (~/Documents/brawl-dolphin/run-lock.py);  ML_LABEL  a label suffix
# Output: $ML_OUT/<tag>/ (the run) and $ML_OUT/<tag>.log; one line per run in $ML_OUT/index.txt.
# Then: node match-latency-report.mjs $ML_OUT/<tag>
set -u
CLASS=$1; TAG=$2; MODE=$3; G=$4; N=$5; shift 5
HERE=$(cd "$(dirname "$0")" && pwd)
OUTROOT=${ML_OUT:-$PWD/match-latency-runs}
LOCK=${ML_RUN_LOCK:-$HOME/Documents/brawl-dolphin/run-lock.py}
: "${ML_BIN:?set ML_BIN}" "${ML_BUILD:?set ML_BUILD}" "${ML_DISC:?set ML_DISC}" "${ML_SESS:?set ML_SESS}"
A=${ML_A:-cy}; B=${ML_B:-di}
if [ "$G" = pplus ]; then
  : "${ML_PPLUS:?set ML_PPLUS}"; SLUG=project-rollback; EXTRA=(--pplus "$ML_PPLUS")
else
  SLUG=open-brawl; EXTRA=()
fi
mkdir -p "$OUTROOT"
OUT=$OUTROOT/$TAG
if [ "$CLASS" = perf ]; then
  pmset -g batt | grep -q "AC Power" || { echo "not on AC power: no timing runs"; exit 3; }
fi
FREE=$(df -g "$HOME" | awk 'NR==2 {print $4}')
[ "$FREE" -ge 10 ] || { echo "only $FREE GB free"; exit 3; }
echo "$(date '+%F %T') start $TAG ($CLASS $MODE $G x$N, $ML_BUILD) $*" >> "$OUTROOT/index.txt"
python3 "$LOCK" "$CLASS" --label "AGENT TEST match-latency $TAG ($MODE $G x$N, $ML_BUILD, $A/$B)${ML_LABEL:+ $ML_LABEL}" --wait-max 10800 -- \
  env ORCA_TEST_NET_DELAY_MS=0 SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_HIDAPI_GAMECUBE=0 SDL_HIDAPI_LIBUSB=0 \
  node "$HERE/match-latency.mjs" --pair --mode "$MODE" --bin "$ML_BIN" --disc "$ML_DISC" "${EXTRA[@]}" --slug "$SLUG" \
  --a "$A" --b "$B" --sess "$ML_SESS" --orca-build "$ML_BUILD" --matches "$N" --out "$OUT" "$@" > "$OUT.log" 2>&1
rc=$?
echo "exit $rc $(date +%T)" >> "$OUT.log"
echo "$(date '+%F %T') done $TAG exit $rc" >> "$OUTROOT/index.txt"
exit $rc
