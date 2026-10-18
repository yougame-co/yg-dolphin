# train-0354 verification

Base: `343a655173 Keep boot-memory test portable to MSVC`; `git merge-base --is-ancestor 343a655173 HEAD` printed `BASE-OK` before any edits. Branch `train-0354`. Local commit only; no version bump, production action, or push.

## Rooms WELCOME contract

```json
{"t":"welcome","lifecycle":"game","room":{"queue":"casual","recovery":true}}
```

The field is **`room.recovery`**, a JSON **boolean**, alongside `room.queue`, `room.stay`, `room.size`, and `room.doubles`. Send `true` only for casual/ranked rooms whose server supports retaining the seat/identity, 90-second away hold, same-ticket rejoin, authoritative away/back/leave roster and authoritative match outcomes. Send it on **every WELCOME**, including rejoin, consistently for both peers. It is per room; never an app hint, ticket flag, environment switch, or inference from the queue. Omit it on today's rooms. Missing, false, null, strings, numbers and objects do not opt a queue in. A queue has no recovery before WELCOME. The existing `recovery_enabled` switch remains required. Private rooms keep the original friends predicate, including the pre-WELCOME friends hint, independently of this field.

A recovery rejoin must confirm the same queue, opt-in, connection identity and seat. It must carry the current `matchId` and roster. Replaying `start` for the same match preserves the tally and sent report identities. Final server `result` (or server `finish` using the same verdict fields: `matchId`, `ranking`, `void`/`draw`, `disposition`, optional `reason`) wins over any active hold. Ranked forfeit uses `disposition:"forfeit"`; no-contest uses `void:true`. `set-over` or close 4004 also terminates a hold without inventing a result. A server-supplied final `reason` becomes the room's final sentence. A refused rejoin/expired hold with no verdict is a network ending; the engine never claims a stalled opted-in ranked set.

## Call-site decisions

Original FriendsRecovery call sites, plus the two mutex-held copies:

| Location/function | Decision |
| --- | --- |
| `Impl::FriendsRecovery` | Friends only; original predicate unchanged. |
| `LinkPeers` | All recovery rooms: retain direct links and pending signaling while room socket is gone. |
| `RoomNotice` | All recovery rooms: room-away/back/leave status lines. Friends-only callers retain their own queue checks. |
| `StartReconnect` | All recovery rooms: 90-second hold, backoff/jitter, close-code finality, same ticket/identity, live input buffers. |
| `OnSocketClose` 1008 / 4001 / 4003 text | Friends only: message-limit/kick/lock wording. Public rooms use the match network sentence; 4004 remains a clean match end. |
| `ConnectRoom` cancellation | All recovery rooms: quit/deadline cancels upgrade. |
| `ConnectRoom` refusal details | Recovery rooms retain refusal details; legacy queue rooms retain the status-only behavior. |
| `BestEffortLeave` | All recovery rooms: bounded teardown, cached ticket only, no new connection except explicit quit. |
| `Run` blackout hook | All recovery rooms: use the real reconnect path; legacy queues still end immediately. |
| `Run` test-drop hook | All recovery rooms: same reconnect path; legacy queues keep their original drop behavior. |
| `Run` dropped best-effort leave | All recovery rooms. |
| `Run` final dropped close and silent-socket branch | All recovery rooms; legacy queue silent-test socket behavior unchanged. |
| `Tick` disconnected early return | All recovery rooms: keep links, defer socket traffic/reports until confirmed WELCOME. |
| `Tick` hello `run` | Friends only: new-process identity supports friends drop-in/host relaunch. |
| `ResetPeer` pending drops cleanup | All recovery rooms. |
| `ResetPeer` retained slot | All recovery rooms. |
| `CheckAwayPeers` | All recovery rooms hold the live session; queue expiry ends the room with network status, while friends keep fresh drop-in behavior. |
| `OnMessage` away/back/join/leave | All recovery rooms: preserve session on away/back, real leave remains authoritative. |
| `OnPeerMessage` new `run` | Friends only: a restarted host/guest uses fresh drop-in. |
| `OnPeerMessage` `resume` | Friends only: recovery queues never rotate or restart the session by this handshake. |
| `HoldInputSilence` predicate copy | All recovery rooms; same deadline and input-buffer policy. |
| `RoomRecovering` predicate copy | All recovery rooms; session stall allowance and overlay deadline. |
| Public `YouGameRoom::FriendsRecovery` accessor | Renamed `RoomRecovery`; exposes shared transport policy. |
| `Online::TestDropRoom` | All recovery rooms retain the existing room object; only legacy silent drops need the retained socket workaround. |

Additional paths: `Tick` pending fresh-drop-in retries are friends only; `WaitForAwayHost` and old-session-stalled host wait remain private-only. Doubles, drop-in rotation, sitting out, lobby/party flows and host relaunch remain private-only. New queue guards preserve report/game-start identities across rejoin, defer unsent reports during a socket outage and preserve ranked game counts on duplicate `start`. Both the transport reporter and the session’s local stall-claim UI refuse to infer a recovery queue forfeit. The native hold notice is published only for opted-in public rooms; friends retain their existing overlay flow. A changed host/seat in a public recovery WELCOME ends with network status instead of entering the friends “open a new room” path.

## Player-visible strings

New engine strings (no version or compatibility-key change):

- `Lost the connection to the match. Press Play to look for a new one.`
- `Reconnecting…` (native queue notice; countdown uses the existing clock renderer).

Reused: `The match is over`; existing ranked/casual result panels, no-contest and forfeit verdicts. Final server `reason` text is supplied by rooms, cleaned and length-limited. The fixture's new verdict sentence is `The other player lost the connection. You win this set.` Existing legacy and friends strings remain unchanged.

## Validation and evidence

All build, suite, syntax-check and fixture commands use `flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10`, at most two build workers. Dolphin peers use isolated users, headless/Null video, the read-only authorized Brawl path and all requested controller-disabling variables. No dependency installs or external services.

- Initial build + `dolphin-nogui`: PASS (`build-orca-tests/train-0354-build.log`, including CTest pass).
- Focused initial recovery/resilience suite: 38 tests, 0 failures (`build-orca-tests/train-0354-recovery-tests.log`).
- Queue fixture self-test, including private behavior and strict queue welcome/drop/rejoin/verdict cases: PASS (`build-orca-tests/train-0354-fixture-self-test.log`).
- Final build (`tests`, `dolphin-nogui`): PASS (`build-orca-tests/train-0354-build-final.log`).
- Full loopback binary: 2,247 tests, 2,224 passed, 23 optional skips, 0 failures; 603.676 s (`build-orca-tests/train-0354-tests.log`, XML `build-orca-tests/train-0354-tests.xml`). This run includes every unchanged friends runtime/outage test. Further final-binary checks include three subsequently added tests and the queue-only native stall-display guard.
- `-Werror`: all five touched C++ translation units PASS (`build-orca-tests/train-0354-werror.log`; exact compile-command syntax checks in `build-orca-tests/train-0354/werror.py`).
- An initial queue fixture attempt used the startup room shortcut and correctly failed its missing-header guard before the match began. Evidence retained in `build-orca-tests/train-0354/casual-startup-attempt/` and its adjacent run log. Queue mode now enters via normal `host`/`join` commands from separate private home rooms. No engine header guard was changed.
- Casual recovery PASS: 16 s socket-only drop; actual same-seat rejoin after 18.172 s; 122 shared checksum frames, last confirmed frame 11,520, zero desyncs. Evidence: `build-orca-tests/train-0354/casual-run.log`, peer logs and `diagnostics.json` in `build-orca-tests/train-0354/casual/`.
- One added runtime test initially used a 13-character fixture room code (protocol limit: 12). It never connected. The test id is corrected to ten characters and its initial wait now prints room status on failure. That attempt is retained as `build-orca-tests/train-0354-tests-invalid-fixture-code.*`; the final full binary is rerun after rebuilding.
- Legacy casual and ranked hold expiry: results appended after completion below.

Fixture events and input scripts are under `build-orca-tests/train-0354/`; all retained peer stderr/stdout and server diagnostics live in that directory. The casual/legacy event drops only peer 1's room socket for 16 seconds, five seconds after the host enters the actual fight. The ranked event drops it for 96 seconds, three seconds after an actual `game-start` report (fighters in); the fixture decides the forfeit at 90 seconds. For ranked, the fixture relays away 250 ms after detection, so its 90-second server verdict is inside the receiver’s 90-second hold without a clock/scheduler race. The socket outage leaves the real loopback direct path intact. The runner checks desync counters, common confirmed checksums, actual same-seat recovery and expected terminal results. It does not assert a verdict for a peer whose socket remains absent through the hold; that peer reports a network end, while the reachable peer accepts the server's forfeit.
