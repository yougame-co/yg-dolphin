# Friends-room recovery: 0.3.44 review fixes

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
