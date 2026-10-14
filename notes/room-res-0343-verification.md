# Friends-room recovery: 0.3.44 review fixes

The original room-fix record below is historical. The room-fix2 follow-up at the end
supersedes its refusal, budget and queue-scope details.

Private branch `room-res-0343`. Starting base check:

```text
2f3b51e34e Resume held-host sessions through drop-in
BASE-OK
```

The starting checkout was clean and contained `2f3b51e34e`. All changes and commits are local. The original room-port brief and review checklist were read, along with the rooms Worker's welcome/roster implementation at the requested read-only path. No release operation was performed.

## Changes by review item

1. **Recovered teardown.** Successful welcome clears the room thread's `dropped` flag. A later room-thread End takes the ordinary FlushLobby/bye/leave path. `Source/Core/Core/Orca/Session/YouGameRoom.cpp:3918`. The runtime teardown test waits for a reconnect welcome/pong, injects a host desync drop, and observes both bye and leave before calling Leave itself (`Source/UnitTests/Core/Orca/RoomTest.cpp:1774`).

2. **Bounded reconnects and final closes.** `RoomResilience.h:72` classifies 1008, 4000, 4001, 4003 and 4004 as final; `YouGameRoom.cpp:1964` gives the friends-room closures their own reasons. `RoomResilience.h:91` holds the per-room rolling budget, and `YouGameRoom.cpp:2024` applies it. At most six accepted reconnect upgrades per five minutes; failed upgrades remain bounded by the original 90-second outage deadline. Welcome clears the outage state but never this budget. `RoomResilience.h:117` applies 70–130% backoff jitter from a room-owned seed. Tests cover final close frames through Run, six successful recoveries followed by a rejected seventh, rolling budget expiry and jitter bounds.

3. **First-connect 409.** `YouGameRoom.cpp:1991` retries only a recognized seat-in-use reason, with three retries after the first refusal, at approximately 0.5/1/2 seconds plus jitter. Full and complete end immediately. `YouGameRoom.cpp:2095` reads recognized refusal reasons; this libcurl stops before delivering the refusal body, so a bounded HTTP Upgrade probe reads the same endpoint's reason. The probe never prints arbitrary response text, and is disabled for queue rooms and quit. Tests exercise actual loopback HTTP 409 bodies, attempt counts, eventual success and immediate full/complete refusal.

4. **Friends-room scope and harnesses.** `YouGameRoom.cpp:1038` gates recovery on the authoritative private queue. A known queue request suppresses first-connect retries until welcome establishes the queue; private welcome overrides an initial queue hint. Queue sockets, away/back resets and healthy hellos retain the train's behavior. Queue request hints are passed in `Source/Core/Core/Orca/Session/Online.cpp:285` and OnlineMatch's room entry paths. Harness opt-in is set in `Tools/orca/dropin-rooms.py:99`, `Tools/orca/pause-room.py:47`, and `Tools/orca/queue-e2e.mjs:470`. Friends-room harness assertions now expect background same-room recovery. The hook remains inert without its opt-in. Runtime tests cover both queues, the inert hook and a private welcome overriding a queue hint.

5. **Quit cancellation and ownership.** `YouGameRoom.cpp:1637` bounds reconnect ticket HTTP waits to two seconds; the stdio ticket request at line 1646 checks stop/leave/cancel every bridge wait. The reconnect socket upgrade is capped at two seconds and checks cancellation. `Source/Core/Common/WebSocket.cpp:76` supplies the handshake cancellation callback. Jobs remain owned and joined; no thread is detached. Runtime tests quit during both an upgrade and ticket refresh, and verify prompt completion.

6. **Quit leave.** `YouGameRoom.cpp:2140` makes one cached-ticket connection attempt, sends leave and closes within a shared two-second attempt deadline. It does not refresh the ticket or retry this attempt. `WebSocket.cpp:264` permits the close wait to use the remaining time. Both quit runtime tests observe leave at the fixture.

7. **Actual welcome shape.** `YouGameRoom.cpp:3834` finds `away` on `players[]` by player id. The corrected test at `RoomTest.cpp:1671` puts it only there, with ordinary unmodified participants. It retains the peer at 10 seconds and ends its hold at 90 seconds.

8. **Both transports silent.** `YouGameRoom.cpp:4487` grants a bounded hold during the existing stall even before the server has sent away. The original deadline cannot be restarted by polling. `Source/Core/Core/Rollback/SessionPort.cpp:174` asks the transport whether to keep waiting after the existing timeout; `Source/Core/Core/Rollback/OnlineMatch.cpp:4045` also keeps the stall budget from ending a held friends session. A real leave, hold expiry or final room end interrupts it. Buffered inputs resume the same Session. A missing input prefix or a frame gap beyond the input window queues a fresh handshake by connection id in `YouGameRoom.cpp:4840`, preserving the room code. The existing host wait and keyframe path handles this resync. The real Run/reconnect/Session/StepSession fixture blocks room upgrades and inputs for 15 seconds without a peer-away verdict and verifies continuation, no leave and unchanged code. A second test injects an unavailable input gap, observes the resync event and receives the existing keyframe offer in that same room.

9. **Retryable resume.** `YouGameRoom.cpp:4480` resolves retry requests to stable roster ids; Tick handles those ids at line 3297. Resumes carry an optional per-request `resume_id` and retry with backoff until a keyframe arrives or the hold expires. Host deduplication at line 4212 distinguishes retries from a new resume cycle, including a host that already has a live session and an old keyframe offer. `OnlineMatch.cpp:1781` clears the host wait only when a keyframe arrives. Runtime tests exercise repeated resume messages, the keyframe response, and a second resume request against an already resumed host.

## Determinism and compatibility

Compatibility remains 30. Session.cpp, simulated timers, rollback decisions and packet encoding are unchanged. The timeout exception belongs to the room transport and existing emulator stall adapter. `run`, `resume` and `resume_id` are optional private-room hello fields; absent fields remain accepted. The runtime healthy test drives two real Sessions with the same varying inputs and compares all 200 frame hashes with recovery enabled and explicitly disabled. The earlier direct-room-blip frame-hash test also passes. Full seamless recovery still requires the new behavior on both machines; older clients keep their old outage policy.

## Final local gates

Every build, suite and syntax check was run under the shared build lock and nice level 10, after verifying load below 20. The required build command passed:

```sh
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 cmake --build build-orca-0343 --target tests -j4
```

The complete suite ran with one test worker through `Tools/orca/room-fix-tests.py`. This dependency-free runner starts only loopback room/keyframe fixtures and provides a minimal environment without inherited app credentials or live-test opt-ins.

| Tests | Registered | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: | ---: |
| Complete suite | 2,048 | 2,025 | 0 | 23 |
| Orca* | 997 | 974 | 0 | 23 |
| Room resilience + runtime | 41 | 41 | 0 | 0 |

There are 17 additional registrations: 16 runtime checks and the rolling-budget/jitter check. The 24 existing traces were updated for the held stall and real server message shape. Python syntax checks passed for the fixture, dropin-rooms and pause-room; node syntax checking passed for queue-e2e.

`forbidden-calls --baseline-rev train-0343` reports **17 branch-added hits, down from 18 at the starting base**. Its overall existing-baseline report remains nonzero: 106 pre-existing train findings plus 17 from this branch (123 total), versus 124 at the starting base. No new scan allowance or baseline was added.

Evidence: `build-orca-0343/room-fix-build-final.log`, `room-fix-tests.log`, `room-fix-tests.xml`, and `room-fix-forbidden-{train,before,after}.json`.

The 23 skipped cases are the existing opt-in live-service, disc, latency and font/design cases. No local gate is blocked. Emulator-boot integration through OnlineMatch's WaitForAwayHost/PumpJoin and actual keyframe loading/playback was reviewed but not executed: the runtime fixtures reach the room thread, reconnect thread, teardown, real Session/StepSession and keyframe offer. The real two-machine emulator check remains for the release lead. The live dropin/pause/queue harnesses were syntax-checked, not run against external services.

## Release-lead two-machine plan: the actual whole-connection outage

Use two staging 0.3.44 clients in a private friends room, compatibility 30, an active match and a confirmed direct link (`tx=1`). Record the original room code, seat identities, netlogs and confirmed frame hashes. Use an isolated VM/gateway network filter controlled by the release lead; do not substitute a graceful direct-off command for a packet blackout.

1. **Joiner, 15 seconds:** On the joiner's isolated network path, blackhole BOTH its room-host TCP traffic and BOTH directions of the selected peer direct UDP link for exactly 15 seconds. Send no close frame and keep the local app bridge working. The host's own room socket stays healthy. This reproduces the period in which the host has silent peer inputs but no server away verdict. Both games must freeze through the existing stall, stay in the original room, and avoid host-left/new-code recovery at 10 seconds. Restore both paths together. With retained input buffers they must continue the same session; compare confirmed hashes after catch-up.
2. **Host, 15 seconds:** Repeat the identical two-path blackout on the host machine. Verify the same stall, same seats/code, buffered continuation and matching confirmed hashes on both sides.
3. **Joiner then host, 60 seconds:** Repeat both blackouts for exactly 60 seconds, separately on each machine. The original hold must remain alive. Failed upgrades must not consume the accepted-reconnect budget. Restore both paths together before hold expiry. Retained buffers continue; if the missing input range is unavailable, expect the existing keyframe/drop-in progress (`joining N%`) followed by play in the SAME room and matching confirmed hashes. No new invitation code may appear during either hold. Keep the loopback missing-buffer case as explicit coverage when the real outage preserves all buffered inputs.
4. **Final/verdict checks:** During a held stall, have the peer really leave; this must interrupt waiting immediately. Run a blackout beyond the hold to verify a clean end. Check 1008/4001/4003 finality, 1013 recovery, actual seat-in-use 409 backoff, full/complete refusal, and user quit during reconnect with prompt leave.
5. **Healthy/interoperability checks:** Compare healthy confirmed hashes against 0.3.43 and check healthy 0.3.43/0.3.44 pairing. Run the updated dropin-rooms/pause-room harnesses in the lead's authorized isolated setup and the ranked rejoin harness to confirm its original behavior.

This plan is for the release lead. No deployment, release, signing, push or external-service test was performed here.


## room-fix2 final review follow-up

Base check before edits:

```text
7080eee6b7 Update room recovery harnesses and verification
BASE-OK
```

The checkout was clean on `room-res-0343`. All follow-up writes are local to this
worktree. The referenced brief, review and Worker source were read at their explicitly
requested reference paths; the Worker was not modified.

- The first room upgrade now makes one request. Its own WebSocket error path may attach
  recognized full/complete reasons; an unclassified 409 ends as `room_full`. No HTTP
  Upgrade probe remains. Loopback tests check one connection and zero probes, and a
  capture of enabled NETPLAY/COMMON logs covers refusal, timeout and cancellation.
- The stall budget excludes only an ongoing held outage. Completed stalls still count
  toward the rolling budget. A 9.9-second input cadence spends the budget after two
  completed stalls; one held outage remains excused until its verdict/deadline.
- Ranked/casual back/join reset the hello timer and greet on the next Tick. Silent test
  drops leave their sockets open and unread; Online retains their room ownership as on
  train. New room notices and best-effort recovery leave are friends-only.
- Receive ignores an expired input hold when deciding whether buffered inputs need a
  resync. A loopback test returns a large gap after an expired hold and verifies the
  packet reaches the caller without a room-resume departure.
- Recovery teardown sends one short best-effort leave when its socket is open. Final
  teardown with no socket does not create a connection; only a user quit may make the
  existing cached-ticket attempt. An accepted socket still owned by the reconnect job
  is joined and transferred before this teardown. A changed-seat recovery fixture exercises the real
  open-socket final path and checks exactly one leave.

The train-to-working-tree runtime audit also restored queue roster processing for
`leave`, retention of pending drops across peer reset, Tick and direct-signal behavior
on a closed socket, the train test-drop end reason and queue handshake/ticket wait
cancellation policy. Remaining recovery state and optional hello fields are either
friends-gated or inert when recovery is false. Session/packet decisions are unchanged.

Final locked build passed with the required command:

```sh
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 cmake --build build-orca-0343 --target tests -j4
```

The complete suite ran through `Tools/orca/room-fix-tests.py`, under the same lock and
nice level, with one test worker and the existing minimal loopback-only environment.

| Tests | Registered | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: | ---: |
| Complete suite | 2,053 | 2,030 | 0 | 23 |
| Orca* | 1,002 | 979 | 0 | 23 |
| Room resilience + runtime | 45 | 45 | 0 | 0 |
| Stall budget | 2 | 2 | 0 | 0 |

The 23 skips remain the existing opt-in live-service, disc, latency and font/design
cases. The runtime healthy comparison covers all 200 frame hashes with recovery both
enabled and disabled, and the real 15-second both-transport-silent case also passes.
The first build needed test-name qualification after adding the logger header; the
first suite run exposed missing in-memory logger configuration during fixture cleanup.
Both were corrected before the final successful build and complete suite.

Evidence: `build-orca-0343/room-fix2-build-final.log`, `room-fix2-tests.log` and
`room-fix2-tests.xml`. The whole train-to-working-tree diff was checked for logging and
Status paths carrying the constructed room URL: none does. The enabled-log regression
covers refusal (with a probe-delay trap), timeout and progress cancellation, checking
both the query marker and the fixture value.

`forbidden-calls --baseline-rev train-0343 --json` remains at **17 branch-added
hits**, with 106 pre-existing train findings (123 total). Its baseline-excess exit
status is unchanged; no allowance or baseline was edited. Evidence:
`build-orca-0343/room-fix2-forbidden.json`, compared with the existing train and
7080eee6b7 reports.

Current source references by room-fix2 item:

1. Single upgrade: `YouGameRoom.cpp:2096`; recognized own-handshake reasons:
   `Source/Core/Common/WebSocket.cpp:124`; enabled-log regression: `RoomTest.cpp:2332`.
2. Ongoing-hold budget: `OnlineMatch.cpp:4045`, `OnlineMatch.h:269`;
   cadence regression: `Games/Brawl/OnlineMenuTest.cpp:2099`.
3. Queue immediate hello: `YouGameRoom.cpp:3950`; retained ownership:
   `Source/Core/Core/Orca/Session/Online.cpp:808`; queue regressions:
   `RoomTest.cpp:2244` and `RoomTest.cpp:2270`.
4. Receive deadline: `YouGameRoom.cpp:4886`; regression: `RoomTest.cpp:2297`.
5. Friends-only notices/leave: `YouGameRoom.cpp:1866`, `YouGameRoom.cpp:2118`.
6. Final teardown: `YouGameRoom.cpp:2332`; open-socket regression:
   `RoomTest.cpp:2315`; budget test also joins final teardown and checks no new connection.

Basenames above belong to `Source/Core/Core/Orca/Session`, `Source/Core/Core/Rollback`
or `Source/UnitTests/Core/Orca` as applicable. No local gate is blocked. The release-lead two-machine plan above still applies;
no external-service integration, release or push was performed.
