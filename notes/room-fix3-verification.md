# Friends-room recovery follow-up

Branch: `room-res-0343`, local commits only.

Starting checks:

```text
cd4627be73 Update room verification notes
cd4627be73968c7fdd1b69930ad9d16a881d6b3c
BASE-OK
ROOM-FIX2-BASE-OK
```

`git log --oneline -8` contained all three room-fix2 commits. The explicitly requested
`codex-room-fix2.md` record reported completion at this HEAD. The staging record,
both Mac drop logs, review checklist and both earlier briefs were read.

Local implementation commits:

- `6e518fa245` — Preserve room session recovery.
- `82ec0df2fb` — Add scheduled connection test hooks.

## Changes

1. An announced friends-room peer retains its slot/session on a quick `back` or same-id
   `join`, even without recent direct packets and before a stall has requested a hold.
   The back handler grants the input hold immediately, preserving an existing deadline.
   Recovery welcome uses the same rule. Contiguous returning inputs clear the hold;
   a real leave or expiry removes it. Queue handlers retain their previous policy.
2. The real StepSession timeout and cumulative stall-budget callback consult this hold.
   A recovering peer cannot inject a departure event through a quick back, so the host
   keeps its stalled session past the ten-second no-word timeout. Completed stalls still
   count against the existing cumulative budget.
3. A resync arrival carries recovery status through the real hello/event path. HostEvents
   suppresses a fresh start for that arrival, including when the old session is already
   idle. It rejects older keyframes and captures a keyframe at the current match frame
   once rollback settles. Compatibility and packet encoding are unchanged.
4. `ORCA_TEST_BLACKOUT=S1-S2` starts from the first room welcome. During the interval it
   drops direct packets on send and receipt, silently detaches the old room socket exactly
   like the silent-drop hook, and fails reconnect attempts with a network error before
   any upgrade or ticket-refresh request. No direct packets arriving during the interval
   are queued for later delivery. `ORCA_TEST_BLACKOUT_ROOM_AFTER=N` restores the room N
   seconds before the direct path. Timing does not restart after welcome. The parser is
   strict and requires ORCA_SESSION=1. A valid blackout marks the existing test-key bit;
   the healthy peer can use ORCA_TEST_NET_DELAY_MS=0 to carry the same bit.
5. The HTTP refusal offset no longer shadows the room status member. The requested Clang
   shadow scan runs only the six touched implementation/test translation units. Its first
   pass also found a keyframe lambda capture and two stall callback variables hiding outer
   locals in OnlineMatch.cpp; these were renamed without changing their behavior. A test-local
   clock alias was replaced with the existing RoomClock alias.

## Runtime coverage

The loopback runner uses its existing minimal environment and one test worker. New tests
reach the real room Run/OnMessage/Tick/Receive paths, real Session and StepSession, and
HostEvents/StartKeyframe's actual packing, encryption and room offer. The host adapter
supplies a headless game history and initialized Core RAM, without booting a game disc.
Its keyframe is decrypted and unpacked: frame 6000, original history start 1524, current
RAM checksum, unchanged match-state sentinel, and no scheduled fresh start. It checks
both a live old session and an already-idle one. Two-machine game playback remains a
release-lead check.

Further tests cover immediate quick-back/same-id-join holds and real expiry at the original
deadline after a back; twelve seconds of host input
silence followed by real buffered continuation; both queue policies through Run; strict
blackout parsing, missing-hook and out-of-session inertness; blocked room upgrades and
silent socket handling; early room recovery; and actual two-way direct packet drops with
no later delivery. Existing hold-bound, cumulative-budget, healthy-hash and queue tests
remain part of the full gate.

## Two-machine cases

Use the same private room and normal host/joiner launch configuration on both machines.
Verify a match and a direct link are active before room time 75 seconds. Start fresh
processes for each case; keep the room code, seats, confirmed frame/checksum lines and
joining progress in both logs. The healthy process must have neither blackout variable.
Replace the earlier NET_SPIKES/manual test-direct setup with these environment lines.
No firewall or machine settings change is needed.

Common to both processes:

```sh
export ORCA_SESSION=1 ORCA_TEST_NET_DELAY_MS=0
unset ORCA_TEST_BLACKOUT ORCA_TEST_BLACKOUT_ROOM_AFTER
```

Affected process, one case per fresh launch:

```sh
# joiner, 15 seconds
export ORCA_TEST_BLACKOUT=75-90 ORCA_TEST_BLACKOUT_ROOM_AFTER=0
# joiner, 60 seconds
export ORCA_TEST_BLACKOUT=75-135 ORCA_TEST_BLACKOUT_ROOM_AFTER=0
# host, 15 seconds
export ORCA_TEST_BLACKOUT=75-90 ORCA_TEST_BLACKOUT_ROOM_AFTER=0
# joiner, room first: room restored at 78, direct restored at 90
export ORCA_TEST_BLACKOUT=75-90 ORCA_TEST_BLACKOUT_ROOM_AFTER=12
```

Require unchanged room code and seats, no timeout-driven unplug or solo transition while
the hold is alive, and the same match after recovery. Buffered input continuation is
valid; if unavailable, require joining progress and a current-match keyframe. Compare
confirmed hashes after catch-up. A room/socket recovery alone is insufficient evidence.

## Source and test references

All paths below are relative to this worktree.

| Item | Production path | Runtime coverage |
| --- | --- | --- |
| 1. Quick back/same-id join | `Source/Core/Core/Orca/Session/YouGameRoom.cpp:1904` (`BackPeer`), `:3931` (welcome), `:4061` (back/join) | `Source/UnitTests/Core/Orca/RoomTest.cpp:2457`, `:2529` |
| 2. Held no-word timeout | `Source/Core/Core/Orca/Session/YouGameRoom.cpp:4567`; existing `Source/Core/Core/Rollback/OnlineMatch.cpp:4071`, `:4084` use that hold | `Source/UnitTests/Core/Orca/RoomTest.cpp:2477` |
| 3. Current-match keyframe | `Source/Core/Core/Orca/Session/YouGameRoom.cpp:3390`, `:4309`; `Source/Core/Core/Rollback/OnlineMatch.cpp:1319`, `:1450`, `:1526`, `:1565` | `Source/UnitTests/Core/Orca/RoomTest.cpp:2590` |
| 4. Blackout hook | `Source/Core/Core/Orca/Profile.h:132`, `Source/Core/Core/Orca/Profile.cpp:367`; `Source/Core/Core/Orca/Session/DirectLink.cpp:1141`; `Source/Core/Core/Orca/Session/YouGameRoom.cpp:2095`, `:2306` | `Source/UnitTests/Core/Orca/RoomTest.cpp:2414`, `:2549`, `:2571`; `Source/UnitTests/Core/Orca/DirectLinkTest.cpp:549` |
| 5. Shadowing | `Source/Core/Core/Orca/Session/YouGameRoom.cpp:2151`; `Source/Core/Core/Rollback/OnlineMatch.cpp:618`, `:4043`, `:4083` | Six translation units, Clang `-Wshadow-all` |
| Ranked/casual scope | Original queue branch before `BackPeer`; recovering arrival defaults false | `Source/UnitTests/Core/Orca/RoomTest.cpp:2703` plus existing queue regressions |

## Local gates

The required locked build passed:

```sh
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 cmake --build build-orca-0343 --target tests -j4
```

The complete suite passed under the same lock and nice level, with one test worker:

```sh
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 python3 Tools/orca/room-fix-tests.py build-orca-0343/Binaries/Tests/tests --gtest_output=xml:build-orca-0343/room-fix3-tests.xml
```

| Tests | Registered | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: | ---: |
| Complete suite | 2,062 | 2,039 | 0 | 23 |
| Orca* | 1,011 | 988 | 0 | 23 |
| Room resilience + runtime | 52 | 52 | 0 | 0 |
| Blackout parser/inertness | 1 | 1 | 0 | 0 |
| Direct links | 13 | 13 | 0 | 0 |

The 23 skips are the existing opt-in live-service, disc, latency and font/design cases.
The focused host-continuation/current-keyframe rerun also passed 2/2. The first suite
found malformed new fixtures (an oversized recovery packet and missing replay port
metadata); these were corrected, with a real arrival-event wait and a type check when
reading the fixture's mixed room/peer messages. The final suite passed in full.

The six-file syntax shadow scan and final object build passed without diagnostics. The
object build used the normal compile commands plus `-Wshadow-all -Werror=shadow-all` for
Profile.cpp, DirectLink.cpp, YouGameRoom.cpp, OnlineMatch.cpp, RoomTest.cpp and
DirectLinkTest.cpp only. No unrelated translation unit received those flags.

Evidence: `build-orca-0343/room-fix3-{build,shadow,tests,focused}.log` and the test XML
alongside them. All local gates passed; blocked: none. Only release-lead two-machine
verification remains; no local test needs an external service. No push was made.
