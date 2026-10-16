# Friendlies 0.3.48 verification

Train `train-0348`, resumed 2026-10-16 at `15621acde9`; required base `694f5f9644`
is an ancestor (`BASE-OK`). One `build-orca-tests` Ninja Release arm64 build,
tests and NoGUI enabled. All runs use localhost dev tickets, the authorized Brawl
WBFS read in place, isolated temporary users, headless Null video and no sound.
Build/test/fixture commands use the shared flock and nice 10, preflight load <18,
and no more than four game instances. Fixture `finally` cleans up its exact children.

## Fixture correction

The first case-2 attempt was stopped after logs showed `Doubles2` received port 2
and `Doubles1` port 3. Concurrent ticket requests assigned seats in arrival order,
while role-specific pad scripts assumed role order. Three peers synchronized, but
this could not prove the intended fighters/rotation. No game/runtime defect was shown.
Evidence: [attempt 1](friendlies-0348-runs/case-2-attempt-1/peer-0.log),
[peer 1](friendlies-0348-runs/case-2-attempt-1/peer-1.log),
[peer 2](friendlies-0348-runs/case-2-attempt-1/peer-2.log).

The runner now waits for each role's authoritative seat assignment before booting
the next role and checks that it received the intended port. The isolated runner
regression delays the second ticket and checks that the third child cannot start
before it owns its seat: **FAIL before, PASS after (1 test)**.
Evidence: [before](friendlies-0348-runs/role-order-before.log),
[after](friendlies-0348-runs/role-order-after.log).

## Gameplay cases

| Case | Result | Evidence |
| --- | --- | --- |
| 1: three peers, two fight, one sits out | PASS repeated as fight 1 of rotation | case-2-attempt-7/ |
| 2: rotation at next CSS | PASS: ports 1/2 then 1/3, both fights back to CSS, confirmed through 7740, 0 desync | case-2-attempt-7/ |
| 3a: four peers, two sit out | PASS: four synchronized peers, only ports 1/2 fight, back to CSS, confirmed through 9000 | case-3-attempt-1/ |
| 3b: four fight 2v2 | PASS: teams 1+3 / 2+4, both winners have one stock and place 0 on all four peers, back to CSS, confirmed through 9000, 0 desync | case-3-attempt-3/teams-evidence.json |
| 4a: sitting-out peer leaves | PASS: only port 3 leaves; fighters return to CSS, confirmed through 6000, 0 desync | case-4a-attempt-1/ |
| 4b: sitting-out peer away/back | PASS: same seat retained; back to CSS, confirmed through 5940, 0 desync | case-4b-attempt-1/ |
| 4c: sitting-out peer blackout/return | PASS: 50–56 s blackout, same seat, fight back to CSS, confirmed through 5640, 0 desync, one counted stall | case-4c-attempt-4/ |
| 5: third arrival mid-fight | PENDING | |
| 6: third arrival during recovery | PENDING | |
| 7: fifth arrival refused alone | PENDING | |
| 8: ordinary room refuses only third | PENDING | |

The controls and rule setup are under `friendlies-0348-runs/controls/`.
`one-stock.probe` sets stock mode/one stock for finite fights, then reads each
fight's four player records. A fight's mask is checked from those records on
every peer that dumped it, with a host dump required; scene logs must show each
fight returning to CSS. Final confirmation hashes must agree and stats show 0 desync.

## Gates

Initial doubles fixture self-test PASS (four clients, fifth refusal, six pairs,
shared capture, retained-away roster, recovery, leave, expiry and confirmed hashes).
Evidence: [self-test](friendlies-0348-runs/self-test.log).
Final self-test, full unit binary and Orca-filter counts are pending.
No pushes, production operations, external services, signing or mailbox calls.

## Rotation investigation in progress

Attempts 2 and 3 picked up the outgoing player's token with B; this leaves their
human panel active, so Start cannot begin the next fight. Attempt 5 moved that
held token to the portrait area and pressed A: the panel stayed active. Attempt 6
held B over the own player-type button: all peers returned to the main menu,
not a rotation. These are real failed control attempts, with no second fight.
Their first fights used ports 1/2, returned to CSS and stayed synchronized.
Attempt 4 was interrupted by SIGINT to its exact fixture PID when measured load
rose to 18.35; its finally cleanup finished. This is no gameplay verdict.

A regression now exercises A on the outgoing player's own human panel with a
held token and at least three connected ports. It failed against the original
gate (A masked); the experimental gate passes all **20 OrcaOnlineSeats tests**,
including the new regression and the existing two-player/ranked behavior checks.
Evidence: `rotation-test-before.log`, `rotation-test-after.log` under the runs dir.
Live attempt 7 uses this experimental binary. No live rotation PASS yet; do not
claim the change is complete until it works and each remaining case is proved.
No compatibility revision has changed yet. Engine compatibility stays 31; if this
module input change is retained, the affected games need their own revision bump.

The reproducible case runner is `friendlies-0348-runs/controls/run-case.py`.
It uses the shared lock per fixture and rechecks load after acquiring it, reserving
headroom by requiring load below 12 before starting up to four game processes.
The hard limit is 18. Subsequent runs print their exact fixture PID for cleanup.

## Confirmed rotation fix and compatibility

Live attempt 7 PASS: plug frame 1570 for both joiners; first fight humans ports
1/2, second fight 1/3, both back to CSS; matching confirmed hashes through 7740,
0 desyncs. The outgoing player's panel changes from human to empty after A on
the own player-type button while holding the token; it stays empty (area dumps
at CSS2 frames 270/300/400). Existing input handling masked that action.
The fix permits it only for a connected player's own human panel, token held and
not flying, with at least three connected ports, outside a locked header. It
centres the stick on A. No new bench state or rotation messages; native game
panel state and the existing CPU clear do the work. The two-player, ranked and
other-panel gates remain covered by tests.

This changes emulated input handling. The Brawl and Project+ game revisions must
be 1 (`ux=31.1`) to keep the earlier masks from joining these sessions. Engine
compatibility stays **31**; the results-only and legacy-launcher game revisions
stay unchanged. The per-game bump and compatibility assertions are written;
they will be included in the final build/gates.

## Four-peer proof

Attempt 1 PASS for a two-fighter/two-idle fight and a four-human free-for-all,
through confirmed frame 9000, 0 desyncs. It is **not** the 2v2 proof: the team-tab
click missed (cursor target 0). Attempt 2 clicked the native team tab (target 1,
button 3), but the colour inputs made a 1v3 split; likewise not 2v2 proof.
Attempt 3 corrects the colour inputs: fight 2's player records have teams
`[0,1,0,1]`, and all four peers' results have stocks `[1,0,1,0]`, places `[0,2,0,3]`.
Ports 1 and 3 are joint winners. The fight lasts 864 frames, before port 3's
scheduled walk at 1000, and returns to CSS. All four agree through frame 9000;
0 desyncs. Evidence: `case-3-attempt-3/teams-evidence.json`, probe binaries,
scene logs and `run.log`, all under `friendlies-0348-runs/`.

## Blackout fixture setup

Case 4c attempt 1 ended before gameplay with `room_mismatch`. The per-role
blackout knob contributes `test=true` to the compatibility key; only the faulted
peer had it, so the other peers correctly refused that different setup. This is
a fixture setup failure, not a runtime blackout verdict. The fixture now gives
non-faulted peers a valid inactive blackout interval (`1000000-1000001` seconds),
so every peer has the same test setup and only the selected peer loses traffic
during the run. The regression failed before and passes after; **2 Python runner
tests pass cleanly**. Evidence: `blackout-compat-before.log` and
`blackout-compat-after.log`. Attempt 2 is queued.

The game compatibility revisions and updated assertions are now built. **25
selected tests passed** (20 OnlineSeats plus the per-game, legacy key, historical
key goldens, results-only and generic-module key checks). Evidence:
`compat-revision-build.log`, `compat-revision-tests.log` under the runs directory.

## Blackout fixture writer correction — proved

Attempts 2/3 admitted all peers but the local fixture stopped answering room
requests during the blackout. The live SIGUSR1 thread trace in attempt 3 shows
`Rooms.send` blocked in its socket write via `broadcast`, while room ticket and
GET handlers wait for the room lock. The nonreading-socket regression reproduces
that indefinite lock hold (FAIL before). The fixture now applies a one-second
kernel send timeout (reads can remain idle) and closes a socket whose write
fails, allowing its normal away handler to run. All **3 Python runner tests PASS**
afterward. Evidence: `room-backpressure-before.log`, `room-backpressure-after.log`,
`case-4c-attempt-3/run.log`. `--dump-stacks` can reproduce the own-fixture trace
with SIGUSR1; it reads no other process.

Attempt 4 PASS: only peer 3 blacked out at 50–56 s, recovered on the same seat,
and both fighters retained it. The fight returned to CSS; confirmed hashes agree
through 5640, 0 desync, one counted stall. Healthy peers' room connections stay
up with the fixture correction. The fixture prints its exact PID and cleans up
only its own game children in finally. No emulator recovery change was needed.
