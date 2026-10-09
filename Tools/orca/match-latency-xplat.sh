#!/bin/bash
# match-latency-xplat.sh <check|perf> <tag> <casual|ranked> <brawl|pplus> <matches>
# Mac<->PC match timing (ORCA.md, "Match start timing"): one Orca here (player ML_A) and one on the
# PC (player ML_B, over SSH: match-latency-pc.ps1 in ML_PC_DIR). The Mac side holds this Mac's run
# queue and starts the PC side once it has its turn; both search at the same moment (--search-at),
# then play N matches (the joiner leaves after each). MACLAG=<s>: the Mac searches that much later,
# so the PC waits longest and hosts. Output: $ML_OUT/<tag>-mac/ and $ML_OUT/<tag>-pc/ (copied back).
#
# Environment: as match-latency-run.sh (ML_BIN, ML_BUILD, ML_DISC, ML_PPLUS, ML_SESS, ML_A, ML_B,
# ML_OUT, ML_RUN_LOCK), plus ML_PC (user@host), ML_PC_KEY (its SSH key), ML_PC_DIR (the PC folder
# holding match-latency.mjs and match-latency-pc.ps1, C:\orca-ab\mlat) and ML_PC_EXE (the PC's Orca.exe).
set -u
CLASS=$1; TAG=$2; MODE=$3; G=$4; N=$5
HERE=$(cd "$(dirname "$0")" && pwd)
OUTROOT=${ML_OUT:-$PWD/match-latency-runs}
LOCK=${ML_RUN_LOCK:-$HOME/Documents/brawl-dolphin/run-lock.py}
: "${ML_BIN:?}" "${ML_BUILD:?}" "${ML_DISC:?}" "${ML_SESS:?}" "${ML_PC:?set ML_PC=user@host}" "${ML_PC_KEY:?}" "${ML_PC_EXE:?}"
PCDIR=${ML_PC_DIR:-'C:\orca-ab\mlat'}
A=${ML_A:-cy}; B=${ML_B:-di}
if [ "${6:-}" != "--inner" ]; then
  if [ "$CLASS" = perf ]; then pmset -g batt | grep -q "AC Power" || { echo "not on AC power"; exit 3; }; fi
  mkdir -p "$OUTROOT"
  echo "$(date '+%F %T') start $TAG (Mac<->PC $CLASS $MODE $G x$N, $ML_BUILD)" >> "$OUTROOT/index.txt"
  exec python3 "$LOCK" "$CLASS" --label "AGENT TEST match-latency $TAG (Mac<->PC $MODE $G x$N, $ML_BUILD, $A/$B)" --wait-max 10800 -- \
    env SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_HIDAPI_GAMECUBE=0 SDL_HIDAPI_LIBUSB=0 bash "$0" "$@" --inner
fi
pc() {
  local enc
  enc=$(printf '%s' "$1" | iconv -f UTF-8 -t UTF-16LE | base64)
  ssh -i "$ML_PC_KEY" -o BatchMode=yes -o ConnectTimeout=15 "$ML_PC" "powershell -NoProfile -EncodedCommand $enc" 2>&1 |
    grep -v -e '^#< CLIXML' -e '<Objs Version'
}
if [ "$G" = pplus ]; then SLUG=project-rollback; EXTRA=(--pplus "$ML_PPLUS"); else SLUG=open-brawl; EXTRA=(); fi
# The PC boots slower: both search 100 s from now.
AT=$(( $(date +%s) + 100 ))
echo "match-latency-xplat $TAG start $(date +%T): both search at $AT"
pc "& '$PCDIR\\match-latency-pc.ps1' -Mode $MODE -Game $G -Out $TAG-pc -Matches $N -SearchAt $AT -Name $B -Exe '$ML_PC_EXE' -Build '$ML_BUILD'; \"EXIT=\$LASTEXITCODE\"" > "$OUTROOT/$TAG.pc.out" 2>&1 &
PC=$!
env ORCA_TEST_NET_DELAY_MS=0 node "$HERE/match-latency.mjs" --side a --mode "$MODE" --bin "$ML_BIN" --disc "$ML_DISC" ${EXTRA[@]+"${EXTRA[@]}"} --slug "$SLUG" \
  --a "$A" --sess "$ML_SESS" --orca-build "$ML_BUILD" --matches "$N" --search-at $((AT + ${MACLAG:-0})) ${MACLAG:+--search-delay $((MACLAG * 1000))} \
  --out "$OUTROOT/$TAG-mac" > "$OUTROOT/$TAG-mac.log" 2>&1
rc=$?
wait $PC
echo "exit $rc $(date +%T)" >> "$OUTROOT/$TAG-mac.log"
mkdir -p "$OUTROOT/$TAG-pc"
PCDIR_FWD=$(printf '%s' "$PCDIR" | tr '\\' '/')
scp -q -r -i "$ML_PC_KEY" -o BatchMode=yes "$ML_PC:$PCDIR_FWD/runs/$TAG-pc/*" "$OUTROOT/$TAG-pc/" 2>&1 | tail -2
scp -q -i "$ML_PC_KEY" -o BatchMode=yes "$ML_PC:$PCDIR_FWD/runs/$TAG-pc.log" "$OUTROOT/$TAG-pc.log" 2>&1 | tail -2
echo "$(date '+%F %T') done $TAG mac exit $rc, pc $(grep -o 'EXIT=[0-9]*' "$OUTROOT/$TAG.pc.out")" >> "$OUTROOT/index.txt"
echo "match-latency-xplat $TAG done $(date +%T): mac exit $rc, pc $(grep -o 'EXIT=[0-9]*' "$OUTROOT/$TAG.pc.out")"
