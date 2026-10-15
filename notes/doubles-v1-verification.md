# Friends-room doubles v1 verification

Starting branch: `doubles-v1`. Base check, before edits:

```text
3bfe032623 Set version 0.3.44 and add player notes
BASE-OK
```

The initial untracked `imgui.ini` is outside this change. All commits are local.
The design was written and committed first in `notes/doubles-v1-design.md`.

Resume base check (before any further edits):

```text
0f9062abbb Record friends-room doubles v1 design
BASE-OK
```

## Implementation map

| Brief item | Implementation |
| --- | --- |
| Batched arrival and late-friend hold | `Source/Core/Core/Rollback/OnlineMatch.cpp:1307` (`HostEvents`), `:5022` (`FriendsBatch::Ready`) |
| One complete port-values roster and shared capture | `Source/Core/Core/Rollback/OnlineMatch.cpp:1634`; `Source/Core/Core/Orca/Session/YouGameRoom.cpp:893` |
| Same-frame plug, Start gate and worst-member allowance | `Source/Core/Core/Orca/Session/Session.cpp:397`, `:606` |
| Per-recipient inputs and fair history service | `Source/Core/Core/Orca/Session/Session.cpp:1608`, `:1651`; `Source/Core/Core/Orca/Session/YouGameRoom.cpp:5080` |
| Six direct pairs, independent pair fallback | `Source/Core/Core/Orca/Session/DirectLink.cpp:1163`, `:2162`; existing per-pair TURN ownership retained |
| Selective recovery holds and same-room resync | `Source/Core/Core/Orca/Session/YouGameRoom.cpp:4695`; `Source/Core/Core/Rollback/OnlineMatch.cpp:5042` |
| Optional mode and compatibility | `Source/Core/Core/Orca/Session/YouGameRoom.cpp:4939`; only private welcome `room.doubles: true` enables it; compatibility remains 30 |
| P+/Brawl names and controls | `Source/Core/Core/Rollback/OnlineMatch.cpp:3163` and the existing four-port writer in `Source/Core/Core/Orca/Games/Brawl/NameTags.cpp:364`; complete per-seat controls are included in each batch offer. Team/stock/time selection stays in the native casual CSS. |

New coverage is in `Source/UnitTests/Core/Orca/SessionTest.cpp:4702`,
`Source/UnitTests/Core/Orca/RoomTest.cpp:3073`, and
`Source/UnitTests/Core/Orca/DirectLinkTest.cpp:353`. The two-player case compares
every encoded packet and every simulated frame hash with the new path enabled
and disabled; existing healthy room-session hash regressions remain in the full suite.

## Local gates

All builds, suites and shadow checks use the shared build lock, nice 10, with load
checked first. Tests use one worker and the existing minimal fixture environment.

```sh
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 cmake --build build-orca-0343 --target tests -j4
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 python3 Tools/orca/room-fix-tests.py build-orca-0343/Binaries/Tests/tests --gtest_output=xml:build-orca-0343/doubles-tests.xml
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 python3 build-orca-0343/doubles-shadow.py
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 python3 Tools/orca/dropin-rooms.py --doubles --self-test
```

The required locked build passed. The complete suite registered **2,089** tests:
**2,066 passed, 0 failed, 23 skipped** (280.047 seconds). The skips are the existing
opt-in live-service, disc, latency and font/design cases. The focused set passed
**18/18**, including simultaneous retained-peer recovery, shared-capture ownership,
prediction-time checkpoint capture, four-seat frame hashes and identical two-player
packet bytes. No live service was used.

Clang `-Wshadow-all -Werror=shadow-all -fsyntax-only` passed on all **8** touched
translation units (including their headers), with no warnings. The updated
`dropin-rooms.py --doubles --self-test` passed: **4** loopback clients, **6** pairs,
one shared capture, same-seat reconnect and isolated friend departure. All local
gates passed; nothing is blocked locally.

Evidence: `build-orca-0343/doubles-build-resume.log`, `doubles-focused-resume.log`,
`doubles-focused-resume.xml`, `doubles-tests.log`, `doubles-tests.xml`,
`doubles-shadow-resume.log` and `doubles-harness-resume.log`.

## Four-instance harness

`Tools/orca/dropin-rooms.py --doubles` dispatches to `doubles-rooms.py`. The latter
has no external dependencies and uses isolated temporary user folders, explicit
dev identities and no inherited app credentials. Its fixture routes peer messages,
retains away seats, serves shared captures in memory and uses no live service.
The self-test uses four real loopback WebSocket clients, all six pair directions,
one shared capture, an ungraceful disconnect/back and a single friend leave.

Start the lightweight dev fixture, then run the emulator harness under the lock:

```sh
python3 Tools/orca/dropin-rooms.py --doubles --serve --listen 127.0.0.1 --port 8765
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 python3 Tools/orca/dropin-rooms.py --doubles --run --binary <nogui> --disc <disc> --site http://127.0.0.1:8765 --roles 0,1,2,3 --seconds 180 --keep-logs build-orca-0343/doubles-run
```

For the lead's Mac/PC run, bind the fixture to an isolated LAN address and pass
that address as `--site` on both machines. Launch role 0 first; it must own the
host slot before any friend connects. Run `--roles 0,1` on Mac and `--roles 2,3`
on PC. Start the friends together to fill the two-second batch window. The runner
checks each joiner's arrival-assigned port and common plug frame; compare the logs across
both machines as well. `--profile PPLUS32` or `--mod-dir <prepared-bundle>` selects
P+; leave these absent for vanilla Brawl. `--input <script>` supplies scene inputs.

Stdin lines are `<role> <existing-command>`, forwarded only to that exact child.
Examples: `3 test-drop-room`, `3 leave`, `2 stats`, `0 quit`. Exit/cleanup signals
only owned child PIDs. The harness accepts loopback or isolated LAN fixture URLs.

## Remaining lead checks

The real two-machine emulator run was deliberately not executed. Verify four
fighter inputs, independent controls/tags, team selection, port/team colours and
stock/time settings in both games, plus confirmed RAM hashes under a blackout,
resync and friend departure. Real TURN service use and the weakest PC's rollback
cost remain unmeasured. Windows MSVC `/W4 /WX` is a lead-side gate; this Mac runs
clang `-Wshadow-all`. Permanent host departure still ends the hosted session;
host migration, rotation, ranked, queue rooms and replay streaming are unchanged.
