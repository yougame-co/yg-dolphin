Base check before edits: `6ad37fe3e1 YG Dolphin 0.3.85`; `BASE-OK`.
No version bump. Engine repository only; no push, PR, deployment or signing.

Part A accepts casual doubles with four seats, retains the raw casual queue in the room mirror,
and uses the friends gameplay/reporting policy so singles rules/results cannot run it.
Ranked doubles ends once with `Room: ranked doubles needs a newer YG Dolphin`.

| Change | Commit | Code / failing-before regression |
| --- | --- | --- |
| Casual four-seat admission, ranked refusal, friends gameplay/reporting | `31453505c7` | `Source/Core/Core/Orca/Session/YouGameRoom.cpp:5260`; `OrcaRoom.QueueDoublesWelcomeAndRankedRefusal` and `OrcaRoomRuntime.CasualDoublesUsesAllFourSeats` |
| Complete seat/team map, invalid-map latch and status | `10646e30db` | `Source/Core/Core/Orca/Session/YouGameRoom.cpp:4752`; `OrcaRoom.ParticipantTeamsFollowWelcomeLobbyAndStart` and `OrcaRoom.InvalidOrPartialTeamsAreIgnoredForTheRoomOnce` |
| Refusal expectations aligned with an ended room | `c1288fab39` | `Source/UnitTests/Core/Orca/RoomTest.cpp`; `OrcaRoom.CoPageMirrorRequiresFriendsDoublesAndAgreedCap` and `OrcaRoom.QueueDoublesWelcomeAndRankedRefusal` failed in `build-doubles-0386/gates/first-full.xml` |
| Numeric ticket ls, pending local add, casual doubles co-seat gates | this commit | `Source/Core/Core/Orca/Session/YouGameRoom.cpp:1680`; `OrcaRoom.QueueDoublesTicketSeatsIncludeAnAddBeforeWelcome` and `OrcaRoomRuntime.TicketRequestsPublishActualLocalSeatCount` |

The failing-before run used the baseline engine binary with new test scaffolding. Its XML is
`build-doubles-0386/gates/before.xml`: the six
regressions listed above failed against the base behavior. The full initial focused run had
11 tests, 10 failures and one pass, including three exploratory Part B failures and one corrected
room-code-length fixture. The reporting regression was strengthened to check casual `finish`
messages too. Current team coverage also includes four seats and a remote co-seat; ignored team
fields are removed from the page mirror.

A2 stops at map retention and one status line, as permitted. Automatic per-seat colours need new
frame-synchronized guest writes, values acknowledgement and replay/keyframe coverage. No such
writes were added. Friends doubles already leaves Team battle and colours to the host on the
native four-port select: `notes/doubles-v1-design.md:64-70` and the read-only co-seat design's
section 3.4. The session-facing APIs are `YouGameRoom::Teams()` and `Online::RoomTeams()`.

The rooms contract was read from YouGame `origin/claude/v2` at `a9b02d571`, confirmed to contain
`b90186ec1`. `docs/DOUBLES.md:21-31,68-86` describes the flow, participant teams and numeric ls.
At `68ccc21d5`, `rooms/src/index.ts:1829-1836` publishes teams keyed by connection ID, only for
complete rosters; `rooms/src/matching.ts:591` defines the casual/partner mode regexp, and
`rooms/src/game-lobby.ts:71-79` permits a doubles lobby with up to two local players.
There is **no ls reader** in `rooms/src` or the ticket route at that pinned ref or the inspected
local `origin/claude/v2`. The engine emits numeric 1/2 per DOUBLES.md; the upstream reader and live
queue integration remain to be coordinated. No external services were contacted.

Part B stopped at the allowed layout boundary. The existing choices span two native pages.
The unused Spectator (exit 29) and Options label slots are 128x24 and 128x32; the two parent labels,
including With Friends, are 176x80. Both Brawl and Project+ probes confirmed those dimensions
(IA4 in Brawl, RGB5A3 in Project+). A With-Friends-sized fourth pick requires native layout/UV or
resource-binding changes. Replacing only the strip's label would violate the requested size and
the pack's uniform scale/aspect contract (`OnlineMenuTest.cpp:527-530`). No menu patch, label or
unused doubles cap was shipped; the exploratory B tests were removed.
The metadata evidence is `build-doubles-0386/gates/{RSBE01,PPLUS32}-menu-assets.json`.

Validation:

- Baseline Mac configure and `tests dolphin-nogui` build passed, parallel 2.
- Final Mac rebuild of `tests dolphin-nogui`: passed, parallel 2.
- FULL Orca/Broadcast suite: **1,490 passed, 23 opt-in skips, 0 failures/errors** (1,513 cases, 150 suites). The skips are live-service, real-disc, bridge-latency and design-dump opt-ins; the required local disc boots ran separately. The first full attempt found the two refusal-test expectations corrected in `c1288fab39`; the complete rerun passed. Evidence: `build-doubles-0386/gates/full.xml` and `final-gates.log`.
- RSBE01/PPLUS32/PPLUS321: **601 baseline hashes compared per profile, frames 0-600, 0 differences in all three**. No explanations or exclusions were needed.
- The suite filter includes `*Orca*:*Broadcast*`, covering the four Broadcast suites without Orca
  in their names as well as parameterized Orca suites; loopback room/keyframe fixtures are active.
- Baseline RSBE01/PPLUS32/PPLUS321 each exited 0 with all 601 hashes for frames 0-600.
- keyCode before/after: `f86645e20656efe5`, unchanged. The legacy compatibility key moves with the
  engine commit. Part B stopped, so profile patches and the texture tree remain unchanged.
- All build/test/emulation jobs use the requested full heavy-slot path, label
  `orca:codex-doubles-0386`, shared flock and nice; builds use two workers, suites one, emulators
  sequentially. Game assets are read-only and user/cache artifacts are inside this worktree.

Gate logs, one-off scripts and source checkpoints are retained under `build-doubles-0386/gates/` (ignored build artifacts). All engine changes and this record are committed locally.
