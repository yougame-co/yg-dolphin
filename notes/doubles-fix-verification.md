# Friends-room doubles review follow-up

Private local work on `doubles-v1`; no push or external service.

Before edits:

```text
3137e70768 Add four-instance doubles harness and verification
BASE-OK
```

The pre-existing untracked `imgui.ini` is excluded. In the table, Session.cpp and
YouGameRoom.cpp are under `Source/Core/Core/Orca/Session/`; OnlineMatch.cpp is
under `Source/Core/Core/Rollback/`. Test files are under `Source/UnitTests/Core/Orca/`.

## Regression evidence

`build-orca-0343/doubles-fix-baseline.xml` records the review regressions against
`3137e70768`'s Session implementation and the original Boundary selector/wait
policy. Nine of ten tests fail there, including all four required items. The
mesh uses independent directed pair delays, drops packets in flight during a
blackout, and encodes/decodes every delivered packet. Tests check both orders of
host/friend receipt and compare every final frame hash and controller-presence
mask after recovery/departure, including two retained rebuilds before the prior
buffered controller prefix ends. Start masking is compared after applying the
gate to both ordinary late inputs and repeated completed-prefix copies.
The working sources were restored after the reference run.

The additional `RebuildingSeatAcknowledgesItsCompletedPrefixAcrossChunks` case
also fails before its acknowledgment change (`doubles-fix-chunk-before.xml`). It
covers two- and four-player rooms with different inbound/outbound delays, repeated
rebuilds, and a completed prefix longer than two input packets. The rebuilding
seat acknowledges that prefix using its otherwise-unused own ack entry; ordinary
packets keep that entry at -1. Two-player broadcasts include the rebuilding seat
when choosing the oldest needed chunk.

The four-player runtime fixture maintains three direct peers with different send
cadences. Its away/back messages preserve all four participants. It waits for the
returning peer's entire prefix before advancing the fixture's input range, and
aggregates recovery credit across multiple StepSession returns. The first full
fixture run exposed the original two-seat message in this new four-seat fixture;
that fixture-only failure was corrected before the final gate.

| Item | Implementation | Test (suite `OrcaDoubles` unless stated) |
| --- | --- | --- |
| 1: seat departure and retained rebuild | `Session.cpp:166` / `:336` / `:394` / `:449` / `:543` / `:1752`; `OnlineMatch.cpp:1364`, `:1639` | `UnevenBlackoutDropKeepsEverySurvivorHash`, `UnevenRetainedRecoveryKeepsEverySurvivorHash`, `RetainedRebuildFinishesItsBufferedInputsBeforeTheNewStay`, `TwoRetainedRebuildsPreserveBufferedPortHistory`, `UnevenRequestedLeaveForwardsTheCompletedPrefix`, `RebuildingSeatAcknowledgesItsCompletedPrefixAcrossChunks` |
| 2: Start gate edges | `Session.cpp:432`, `:449` | `SeatedStartAcrossUnevenBatchArrivalAndDrop`, `StartGateComparesLatePadsAndRetiredCopiesAfterMasking` |
| 3: stall selection and empty fallback | `Session.cpp:291`; `OnlineMatch.cpp:5035`, `:5043`, `:4378` | `CatchingUpStallNamesItsMissingSeatWithUnevenLatency`, `LateSeatStallNamesMissingPeerWithUnevenLatency`, `BoundaryEmptyMissingSelectionUsesOrdinaryRecovery`, `BoundaryDropsOnlyMissingFriendsAfterUnevenBlackout` |
| 4: await the host's roster | `OnlineMatch.cpp:4180`, `:5052` | `BoundaryWaitsForHostRosterAfterAnotherFriendsHoldExpires` |
| History size at its producer | `Session.cpp:1786` | `BatchHistorySenderCountsOnlyTheFramesItEmits` |
| Pending data across fan-out changes | `YouGameRoom.cpp:4996`, `:5012` | `OrcaDoublesTransport.SwitchingFanoutKeepsPendingHistoryAndInputs`, `BroadcastHistoryReachesANewRecipientDuringTheSwitch` |
| Seated input retention | `Session.cpp:932` | `DelayedBatchRosterRetainsSeatedLocalInputs` |
| Seated peer RTT in plug margin | `Session.cpp:676` | `BatchPlugMarginIncludesSeatedPeers` |
| Legacy wire identity | unchanged absent-field encodings | `OrcaLegacy044.PacketAndKeyframeOfferGolden`, `WelcomeAndHelloGolden`, `InitialSessionPacketGolden`, `SessionPacketStreamGolden` |
| Legacy admission | `OnlineMatch.cpp:1339` | `OrcaRoomRuntime.Legacy044PeerInDoublesIsRefusedWithRoomFull` |
| 0.3.44 recovery in doubles | real room/direct transports and StepSession callbacks | `LegacyAndDoubles/OrcaRoomStallRuntime.*`, instantiated for legacy, doubles two-player, and doubles four-player rooms |
| Six direct and TURN pairs | local vendored TURN fixture; production keeps `NO_SERVER` | `OrcaDirectLink.FourFriendsSendOnlyToTheRequestedSeat`, `FourFriendsExerciseAllSixTurnPairs` |

## Legacy reference

The golden reference compiles `3bfe032623`'s Session and YouGameRoom implementations
against the current test scaffolding. Link-only adapters supply subsequent public
methods and the checksum map's pair key; these do not run in the golden cases.
The test inputs contain no doubles fields. All four reference tests pass. A 400-tick uneven-link stream of 3,798 packets
provides a fixed byte digest as well as explicit hello, keyframe and initial
packet fixtures and welcome outcome assertions. Reference script and evidence
are under `build-orca-0343/doubles-golden-reference*`.

## Harness

`dropin-rooms.py --doubles` now permits direct links. The test-only
`Binaries/Tests/orca-turn-fixture` uses the already vendored library, binds to the
fixture's loopback/LAN address and has a public throwaway identity. Pass
`--turn-binary <fixture>` to `--serve`, and `--transport turn` to both runners to
exercise TURN. No install, real TURN service or real credentials are used.

`--hold-expiry` disables role 3's direct link and room connection; the fixture
retains its seat for 90 seconds, then expires it. Use at least 120 seconds.
Both machines publish their observed plug frames to the same fixture; all four
reports must agree. The runner checks every common confirmed frame hash across
remaining peers and requires at least one common frame. The fixture self-test
also rejects deliberately different plug frames and hashes. Windows inherits
PATH and SystemRoot.

## Gates

All build, test and shadow invocations use the shared lock, nice 10 and one test
worker, after checking load below 20. Build uses the requested `-j4`.
Final results (2026-10-15):

| Gate | Result |
| --- | --- |
| Requested `cmake --build build-orca-0343 --target tests -j4` | Passed |
| Complete ordinary unit run | 2,128 registered; 2,048 passed; 80 skipped; **0 failed** (62.966 s) |
| `python3 Tools/orca/room-fix-tests.py build-orca-0343/Binaries/Tests/tests` | 2,128 registered; 2,105 passed; 23 skipped; **0 failed** (590.890 s) |
| Recovery variants | All 21 `LegacyAndDoubles/OrcaRoomStallRuntime` cases passed |
| `clang -Wshadow-all -Werror=shadow-all` | Clean across all nine affected/related translation units |
| `dropin-rooms.py --doubles --self-test --turn-binary build-orca-0343/Binaries/Tests/orca-turn-fixture` | Passed |
| Golden reference at `3bfe032623` | Four passed; 3,798 encoded stream entries |

The complete ordinary run uses an isolated temporary directory and a minimal
process environment. The room wrapper enables only loopback fixtures. The 23
remaining skips are the existing `OrcaLive` (15), disc/mod (4), UX latency (2),
font (1), and draw (1) cases requiring external game/assets or opt-in hooks.
No external service was used. Evidence is in
`build-orca-0343/doubles-fix-{all,fixtures,shadow,harness}-final.log`, the two
final XML reports (`doubles-fix-all.xml`, `doubles-fix-full.xml`), and
`doubles-fix-final-gates.json`.

No code item remains open. Actual Mac/PC gameplay with a disc was not run;
the cross-machine runner/comparisons are implemented and their local fixture
self-test passes. All six direct pairs and all six real local TURN pairs passed
in `OrcaDirectLink` tests. Windows PATH behavior was changed but not executed on
Windows in this Mac-only run.

All commits are local, with the requested co-author trailer. No push, PR, merge,
release, signing command, installation, or production action was performed.
