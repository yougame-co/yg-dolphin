#!/bin/bash
# The play-again matrix (queue-e2e.mjs --scenario cssjoin / playagain), one run after another, each
# waiting while the owner's own Orca is open (play-again-run-two.sh). Builds: TODAY (a released Orca.app,
# extracted with ditto) and NEXT (a nogui build); ONLY=<specs> and BUILDS=<today next> narrow it.
# Specs: css-ranked, css-casual, css-menu (cssjoin --where), pa-a .. pa-e (playagain --variant), pa-dh
# (d with the set's host inviting: the role swap), pa-d-fl and css-ranked-fl (the friend leaves the
# friends' game instead of the inviter: the inviter's queue pick armed again). EXTRA="--fight ..." goes to every run;
# WAIT_TOTAL (seconds, default 5400) is how long the chain waits for the owner's Orca in all.
# Results: $PLAY_AGAIN_RUN/chain.log (one line per run), outputs in out/, Orca logs under tmp/.
R=${PLAY_AGAIN_RUN:-$HOME/ClaudeWorktrees/orca-play-again-run}
Q=Tools/orca/queue-e2e.mjs
mkdir -p "$R/out"
D=$HOME/Documents/brawl-dolphin/q9-runs/brawl.wbfs
TODAY=${TODAY:-$R/bin-0325b/Orca.app/Contents/MacOS/Orca}
NEXT=${NEXT:-$HOME/Documents/brawl-dolphin/orca-rs-gate/build/Binaries/dolphin-emu-nogui}
START_DEADLINE=$(( $(date +%s) + ${WAIT_TOTAL:-5400} ))
run() { # name bin args...
  local name=$1 bin=$2; shift 2
  [ -f "$R/out/$name.out" ] && grep -q '^exit ' "$R/out/$name.out" && return 0
  local left=$(( START_DEADLINE - $(date +%s) )); [ $left -lt 1800 ] && left=1800
  WAIT_MAX=$left $(dirname "$0")/play-again-run-two.sh "$R/out/$name.out" node $Q "$bin" "$D" --q2 --keep --alarm 900 $EXTRA "$@"
  if [ $? -eq 9 ]; then echo "$(date +%T) $name: gave up waiting for the owner's Orca to close" >> $R/chain.log; exit 9; fi
  echo "$(date +%T) $name: $(grep -E 'RESULT:|PASS:|FAIL:|^exit ' "$R/out/$name.out" | sed -E 's/^\[ *[0-9.]+\] //' | cut -c1-400 | paste -sd'|' -)" >> $R/chain.log
}
for spec in ${ONLY:-css-ranked css-menu pa-d pa-a pa-b pa-c pa-e}; do
  for build in ${BUILDS:-today next}; do
    bin=$TODAY; [ $build = next ] && bin=$NEXT
    case $spec in
      css-ranked) run "$spec-$build" "$bin" --scenario cssjoin --where ranked ;;
      css-casual) run "$spec-$build" "$bin" --scenario cssjoin --where casual ;;
      css-menu) run "$spec-$build" "$bin" --scenario cssjoin --where menu ;;
      pa-dh) run "$spec-$build" "$bin" --queue ranked --scenario playagain --variant d --inviter host ;;
      pa-d-fl) run "$spec-$build" "$bin" --queue ranked --scenario playagain --variant d --leaver invitee ;;
      css-ranked-fl) run "$spec-$build" "$bin" --scenario cssjoin --where ranked --leaver invitee ;;
      pa-*) run "$spec-$build" "$bin" --queue ranked --scenario playagain --variant ${spec#pa-} ;;
    esac
  done
done
echo "chain done $(date +%T)" >> $R/chain.log
