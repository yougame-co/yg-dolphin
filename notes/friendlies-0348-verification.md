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
| 1: three peers, two fight, one sits out | Prior PASS supplied by Luis; not rerun yet | Original task |
| 2: rotation at next CSS | RUNNING (attempt 1 invalidated by role ordering) | friendlies-0348-runs/case-2-attempt-2/ |
| 3a: four peers, two sit out | PENDING | |
| 3b: four fight 2v2 | PENDING | |
| 4a: sitting-out peer leaves | PENDING | |
| 4b: sitting-out peer away/back | PENDING | |
| 4c: sitting-out peer blackout/return | PENDING | |
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
