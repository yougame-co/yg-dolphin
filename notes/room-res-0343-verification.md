# Friends-room recovery on 0.3.43

Branch `room-res-0343`, based on `train-0343` at `7e169aa0a3`. Local commits only. The old reference commit was read and adapted; it was not merged or cherry-picked.

## Behavior

- A1: Peer `away` retains the direct link, peer identity, pending inputs, and session. `back` / same-id `join` clears the mark without another arrival. A real leave still unplugs immediately. Direct input silence uses the existing 10-second limit; a held seat expires after 90 seconds. Duplicate away does not extend it.
- A2: Own socket loss and the existing 8-second server-silence check start a background ticket refresh and WebSocket upgrade for the same room. Direct links and the page's room mirror stay alive. Welcome confirms the same identity and seat. The reconnect deadline never restarts. A silent direct peer ends its session after 10 seconds; unsuccessful room recovery ends the room after 90 seconds.
- A3: A friends-room joiner whose host session ends during a room blip keeps the room, plays solo, and waits for the held host. Back initiates the normal hello / keyframe / joining-percent path. Real leave or hold expiry ends the wait. Back and an old-session stalled notice may cross without abandoning the room. Ranked verdict handling remains on the train's existing path.
- A4: `room-away`, `room-back`, and `room-leave` lines name the subject, kept / ended / retry decision, and cause. Reconnect attempts and final outcomes include attempt number, original close code/reason, and last connection error. Ticket contents and ticket-refusal text are never included.
- B1: 1013, server closes without a code, and receive failures are reconnectable. 4000 (replaced) and 4004 (match over) remain final.
- B2: HTTP 409 during upgrade retries with 0.5, 1, 2, then capped 4-second backoff, within the original hold. Initial upgrade 409 also waits for the held seat; other initial refusals retain existing behavior.
- B3: Every attempt, refusal, welcome, and final outcome is logged with reconnect context.

## Code map

- A1: Source/Core/Core/Orca/Session/YouGameRoom.cpp:3431, Source/Core/Core/Orca/Session/YouGameRoom.cpp:3749
- A2: Source/Core/Core/Orca/Session/YouGameRoom.cpp:1860, Source/Core/Core/Orca/Session/YouGameRoom.cpp:1955, Source/Core/Core/Orca/Session/YouGameRoom.cpp:3608
- A3: Source/Core/Core/Rollback/OnlineMatch.cpp:1219, Source/Core/Core/Rollback/OnlineMatch.cpp:1706
- A4 / B3: Source/Core/Core/Orca/Session/YouGameRoom.cpp:1829, Source/Core/Core/Orca/Session/YouGameRoom.cpp:1844
- B1: Source/Core/Core/Orca/Session/YouGameRoom.cpp:1919, Source/Core/Core/Orca/Session/RoomResilience.h:77
- B2: Source/Core/Core/Orca/Session/YouGameRoom.cpp:1938, Source/Core/Core/Orca/Session/YouGameRoom.cpp:1945, Source/Core/Core/Orca/Session/RoomResilience.h:107
- Hook: Source/Core/Core/Orca/Session/YouGameRoom.cpp:4568

## Compatibility and determinism

Compatibility remains 30. Session packets, rollback algorithms, simulated timers, and game inputs are unchanged. Hello adds optional `run` (room-object identity) and, only during recovery, `resume`. Existing readers ignore unknown fields; absent fields are accepted. The healthy hello's serialized bytes gain `run`, but game frame hashes and inputs remain identical. Healthy interoperability with 0.3.42 is preserved wherever the train already allowed it. An old peer retains its old room-drop policy, so full seamless recovery requires the fix on both machines.

The reference's recovery traces and frame-hash comparison are ported to this policy API. Extra cases cover close classification, initial/refreshed 409, exponential backoff, hold expiry, same-run and legacy hellos, welcome reconciliation, own input silence, and a stalled notice crossing back. External-service/disc integration cases remain opt-in.

## Exact two-machine check for the release lead

Use a private staging/test room with two copies of this build, direct transport active (`tx=1`), a running match, and both machines recording their usual frame hashes. Use the existing stdin test harness with `ORCA_TEST_COMMANDS=1` and `ORCA_TEST_ROOM_DROP=1` on both machines. No hook runs by default, including in release builds.

1. **Host socket:** Send `test-drop-room` to the host's stdin once. Expect the joiner's `room-away peer ... kept`, uninterrupted advancing frames on both, the host reconnecting to the same code/seat, then `back` without a new arrival, reset, or keyframe. Compare shared confirmed frame hashes. Repeat on the joiner's stdin and verify the same invariants.
2. **Room-only blackout / server silence:** In the lead's isolated test harness, block only TCP to the room endpoint on the host for 9 seconds, preserving the direct UDP link and bridge. Remove that test rule. Repeat on the joiner. Expect the 8-second server-silence recovery path, continuous play, and same-seat recovery. Use an isolated harness; no Mac privacy/security settings are needed.
3. **409 seat in use:** Launch the dropping side additionally with `ORCA_TEST_DROP_SILENT=1`, then send `test-drop-room`. The old socket stays unread while reconnect attempts use the same seat. Verify an actual HTTP 409 / seat-in-use log, retry backoff, unchanged original deadline, and successful recovery. If server timing bypasses 409, use the lead's local room fixture to refuse the first two upgrades with HTTP 409 before accepting; do not count a run without a 409 as this check.
4. **1013:** Launch the dropping side additionally with `ORCA_TEST_ROOM_CLOSE_CODE=1013`; send `test-drop-room`. This injects the classification (not a real server close frame). Expect logged close-code 1013, retry, same room/seat, and continuous direct play. Also have the lead's isolated room fixture send a real 1013 close to verify the libcurl receive path.
5. **Away plus silence:** Drop the host's room socket and block both directions of its direct link (`test-direct off` in the harness) for over 10 seconds. The old session must end at the existing timeout, and the joiner must retain the old room code while waiting. Restore direct with `test-direct on` and room reachability before 90 seconds. Expect back, fresh hello/keyframe, `joining N%`, automatic play together, and matching confirmed hashes after rejoin. Repeat with the joiner's room socket dropped. Use a room-only test outage long enough that back follows the silence timeout.
6. **Back / final cases:** Repeat away followed by back before 10 seconds (no reset), after 10 but before 90 seconds (fresh drop-in), and after 90 seconds (wait ends). A duplicate away must not extend the hold. A real leave during away must end the wait immediately. 4000 must not reconnect and 4004 must keep the match-over outcome.

Two-machine checks remain for the release lead; this run contacts no external services and performs no release operation.

## Light-gate results

- Required incremental build: `flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 cmake --build build-orca-0343 --target tests -j4` passed. The final build includes the corrected current-format keyframe fixture and deadline handling.
- Full suite under the same lock and `nice -n 10`, one test worker, minimal environment, and the existing `fake_keyframes.py` loopback fixture: **2,031 registered; 2,008 passed; 0 failed; 23 skipped**.
- Orca: **980 registered; 957 passed; 0 failed; 23 skipped**. All runnable Orca tests passed. All **25 added checks** passed (24 recovery traces/policy checks plus the frame-hash comparison), including the adapted reference checks.
- Skipped: 15 live-room/bridge integration tests, 4 disc/revision tests, 2 latency harness tests, and 2 disc/font/design tests. No real service was contacted. The localhost keyframe-store test ran and passed.
- Counts: the older `baseline-tests.xml` contains 934 Orca registrations (910 passes, 24 skips); the pre-existing `final-tests.xml` contains 955 (931 passes, 24 skips). This change adds 25 registrations to the current train's 955, yielding 980. Running the loopback keyframe fixture removes one baseline skip.
- Final evidence: `build-orca-0343/room-port-build-final.log`, `room-port-tests.log`, and `room-port-tests.xml`. The initial run's single outdated-reference-fixture failure is retained in `room-port-tests-initial.log` / `.xml`; the final complete rerun has zero failures.
- No compat bump, push, deploy, signing, release, or production operation. No remaining implementation/test blocker. The real two-machine forced-drop checks above remain for the release lead.
