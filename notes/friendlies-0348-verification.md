# Friendlies 0.3.48 verification

**Gameplay cases 1–8 PASS**, including every subcase. Train `train-0348`, 2026-10-16.
The required base `694f5f9644` was checked before edits: initial HEAD was
`15621acde9 Record train 0.3.48 configuration blocker`, followed by `BASE-OK`.
The previous missing-submodule and disc prerequisites are resolved.

## Runs and evidence

Evidence paths below are relative to `notes/friendlies-0348-runs/`. Each final run
has a real fixture PASS line, peer stderr/stdout logs, diagnostics and fighter
record probes. Every requested fight returned to character select (CSS), and
all final runs reported **0 desyncs**. Confirmed hashes are compared among the
active peers; after a permanent leave, that means the remaining fighters.

| Case | Result and observed behavior | Confirmed through | Evidence directory |
| --- | --- | --- | --- |
| 1: three peers, two fight, one sits out | PASS repeated: humans on ports 1/2; port 3 idle; back to CSS | 7740 across the two-fight run | `case-2-attempt-7/` |
| 2: rotate at next CSS | PASS: port 2 empties its panel; port 3 picks; fight 2 has humans 1/3 and returns to CSS | 7740 | `case-2-attempt-7/` |
| 3a: four peers, two sit out | PASS: humans 1/2, ports 3/4 idle, all four remain synchronized through the fight and CSS | 9000 | `case-3-attempt-3/` |
| 3b: all four play 2v2 | PASS: teams 1+3 versus 2+4; joint winners 1/3, both with a stock remaining; back to CSS | 9000 | `case-3-attempt-3/`, `teams-evidence.json` |
| 4a: sitting-out peer leaves for good | PASS: peer 3 leaves mid-fight; only that port drops; fighters continue to CSS | 6000 | `case-4a-attempt-1/` |
| 4b: sitting-out peer away/back inside budget | PASS: relay absent for six seconds; direct traffic continues; same seat and peer retained; fight returns to CSS | 5940 | `case-4b-attempt-1/` |
| 4c: sitting-out peer blacks out and recovers | PASS: ORCA_TEST_BLACKOUT at 50–56 seconds; both transports absent; same seat retained; one counted stall; fight returns to CSS | 5640 | `case-4c-attempt-4/` |
| 5: third arrives mid-fight | PASS: newcomer waits; CSS at 4044, port 3 plugs at 4455; original friend plugged at 1570; fighters finish normally | 5940 | `case-5-attempt-1/` |
| 6: third arrives during another friend's recovery | PASS: port 2's relay and direct traffic stop; newcomer arrives; port 2 returns on the same seat; CSS at 4045, newcomer plugs at 4466 | 5580 | `case-6-attempt-1/` |
| 7: fifth arrival | PASS: fifth dev ticket gets HTTP 409; no fifth game process; original four continue through fight and CSS | 6000 | `case-7-attempt-1/` |
| 8: ordinary room, third arrival | PASS: only newcomer reports room_full and never plugs; original pair fight and return to CSS | 5940 | `case-8-attempt-1/` |

The two-fight 2v2 run is checked beyond its human-port mask: all four probes have
teams `[0,1,0,1]`, results stocks `[1,0,1,0]`, places `[0,2,0,3]`. Ports 1 and 3
are joint winners. The fight lasts 864 frames, before port 3's scheduled walk at
1000. See [team evidence](friendlies-0348-runs/case-3-attempt-3/teams-evidence.json).

## Runtime fix: returning a token to sit out

Picking a token up with B leaves the human panel active, which blocks Start.
The existing online input gate also blocked A on the player's own type button,
so the player could not empty that panel to sit out. The new regression in
`Source/UnitTests/Core/Orca/Games/Brawl/OnlineSeatsTest.cpp` failed against the
original gate; the fix passes the full 20-test OnlineSeats suite.
Evidence: [before](friendlies-0348-runs/rotation-test-before.log),
[after](friendlies-0348-runs/rotation-test-after.log).

With at least three plugged ports and no locked header, A may now reach only a
connected player's own human panel while its token is held and not flying; the
stick is centred during A. The native game panel and existing CPU clear make it
empty. No bench state or rotation message was added. The ordinary two-player,
ranked, other-panel, empty-panel and unplugged-port gates remain covered.
In live case 2, the outgoing panel changes from human to empty after this press
and stays empty at CSS2 frames 270/300/400; the next fight has humans only 1/3.

This changes emulated input handling: Brawl and Project+ game revisions become
**1**, making their key suffix `ux=31.1`. An older mask would diverge on the
newly permitted action, so this per-game bump is necessary. Engine compatibility
remains **31**; results-only and legacy-launcher revisions remain unchanged.
Historical key serialization goldens remain pinned after normalizing only this
suffix. **25 selected C++ tests PASS**, including the OnlineSeats, per-game,
legacy/historical, results-only and generic-module key checks:
[build](friendlies-0348-runs/compat-revision-build.log),
[tests](friendlies-0348-runs/compat-revision-tests.log).

## Fixture corrections and regression evidence

- **Ticket order:** a slow second ticket let role 2 claim role 1's port, invalidating
  per-role scripts. The runner now waits for every authoritative seat assignment.
  Delayed-ticket regression FAIL before / PASS after:
  [before](friendlies-0348-runs/role-order-before.log),
  [after](friendlies-0348-runs/role-order-after.log).
- **Blackout setup parity:** the blackout knob adds test=true to the compatibility
  key. Non-faulted peers now get an inactive `1000000-1000001` interval, so only
  the requested peer blacks out while every peer has the same test setup.
  Regression FAIL before / PASS after:
  [before](friendlies-0348-runs/blackout-compat-before.log),
  [after](friendlies-0348-runs/blackout-compat-after.log).
- **Fixture writer:** a nonreading peer blocked a socket write while the room lock
  was held. The live own-fixture SIGUSR1 trace confirms the blocked writer and
  waiting ticket handlers. Writes now have a one-second kernel send timeout;
  failed sockets close and their normal away handling runs, while idle reads
  are unchanged. Nonreading-socket regression FAIL before / PASS after:
  [before](friendlies-0348-runs/room-backpressure-before.log),
  [after](friendlies-0348-runs/room-backpressure-after.log),
  [live trace](friendlies-0348-runs/case-4c-attempt-3/run.log).

The final Python runner suite has **3 passing regressions**. No emulator recovery
change was necessary. The fixture's optional `--dump-stacks` traces only its own
threads on SIGUSR1; it does not inspect other processes.

## Earlier attempts retained honestly

| Attempt | Verdict |
| --- | --- |
| Case 2 attempt 1 | INVALID: role/port race; stopped with SIGINT to its exact fixture PID; finally cleaned its children |
| Case 2 attempts 2/3 | FAIL: B unpicked the token but did not empty the human panel; no second fight |
| Case 2 attempt 4 | NO VERDICT: load rose to 18.35; stopped exact fixture PID 44555; cleanup complete; see load-stop.txt |
| Case 2 attempt 5 | FAIL: A on the portrait area did not empty the panel |
| Case 2 attempt 6 | FAIL: held B on the own panel backed the room to the main menu |
| Case 3 attempt 1 | PASS for 3a and a four-human free-for-all; not 3b: team-tab click missed |
| Case 3 attempt 2 | Not 3b: team tab clicked, but colour inputs made 1v3 |
| Case 4c attempt 1 | Setup FAIL before gameplay: mixed test flags caused room_mismatch |
| Case 4c attempts 2/3 | Fixture FAIL: blocked writer made diagnostics/tickets time out; attempt 3 has the live trace |

All these logs remain under the runs directory. No failed attempt is used as a
final gameplay PASS.

## Reproduction and machine rules

Use `python3 notes/friendlies-0348-runs/controls/run-case.py <case> <new-attempt>`;
cases are `2`, `3`, `4a`, `4b`, `4c`, `5`, `6`, `7`, `8`. Existing attempt
directories are never overwritten. `arguments.json` records each invocation;
controls, scheduled events and probes are in `controls/`. The one-stock setup
writes only fixture rules; fighter/team/results dumps are read-only evidence.

One Ninja Release arm64 build at `build-orca-tests`, tests and NoGUI ON, Qt,
bundle and signing OFF; binary `build-orca-tests/Binaries/dolphin-emu-nogui`.
Vanilla Brawl, no launcher profile. Authorized disc, read in place, never copied:
`/Volumes/YGWorktrees/OSCOO/ROMs/Wii/Super Smash Bros. Brawl (USA, Canada) (Rev 2).wbfs`.
Only headless Null video, muted audio and isolated temporary users; no installed
apps, personal profiles, controllers or adapter changes. No disk image attached.

Every fixture/build/test ran under the shared flock and nice 10, builds at most
six jobs, tests one worker, one fixture at a time, at most four game instances.
The runner rechecks load after acquiring the lock; it reserves headroom below
18 (initially 12 for all fixtures, later 14 for up to three roles / 12 for four).
It releases the lock without starting a fixture if that check fails. Exact own
fixture PIDs are printed; finally quits/waits/kills only its own children.
No heavy-slot call, push, external service, production operation, signing or
mailbox call. Final version-0.3.48 gates passed: full binary 2,157 passed / 21
guarded skips / 0 failures; Orca filter 1,085 passed / 21 guarded skips / 0 failures;
doubles self-test PASS; compatibility template 78/78; Python regressions 3/3;
four touched C++ translation units compile with -Werror. See
[train verification](train-0348-verification.md) for commands, logs and skip reasons.
