#!/bin/bash
# One local, muted four-engine run. The caller starts no other heavy work until it exits.
set -euo pipefail
cd "$(dirname "$0")/../.."
export TMPDIR=/Volumes/YGWorktrees/tmp/orca-doubles-teams
proof=notes/doubles-teams-proof
pair=${1:?pair12, pair13 or no-teams}
args=()
guest="$proof/guest.txt"
case "$pair" in
  pair12) args=(--ranked-doubles --teams 0,0,1,1); guest="$proof/guest-pair12.txt" ;;
  pair13) args=(--ranked-doubles --teams 0,1,0,1) ;;
  no-teams) args=(--room noteams4) ;;
  *) exit 2 ;;
esac
uptime
mkdir -p build-doubles-teams/proof-scratch "$proof/$pair"
YG_MIN_DISK_FREE_GB=17 bash /Users/luis/Code/brawl-dolphin/notes/lead-records/yougame-v2-org/tools/heavy-slot.sh run \
  --pool gpu --prio 1 --label orca-doubles-teams -- \
  flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 \
  python3 /Users/luis/Documents/brawl-dolphin/run-lock.py check -- \
  python3 "$proof/doubles-rooms.py" --doubles --run --local server \
  --binary build-doubles-teams/Binaries/Orca.app/Contents/MacOS/Orca \
  --disc '/Volumes/YGWorktrees/OSCOO/ROMs/Wii/Super Smash Bros. Brawl (USA, Canada) (Rev 2).wbfs' \
  --profile PPLUS321 --pplus 'build-doubles-teams/discovery-native/game/Project+ Netplay Launcher.dol' \
  --input "$guest" --input-role "0=$proof/host.txt" --probe "$proof/proof.probe" \
  --seconds 180 --scratch "$PWD/build-doubles-teams/proof-scratch" --keep-logs "$proof/$pair" \
  --expect-fight 0xf --expect-fight 0xf "${args[@]}" > "$proof/$pair/wrapper.log" 2>&1
