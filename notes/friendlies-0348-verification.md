# Friendlies 0.3.48 verification

Task `friendlies-0348`, 2026-10-16. **Blocked at preflight; gameplay is unproven.**

Required base check, before edits:

```text
1588a81517 Set version 0.3.47 and player notes
BASE-OK
```

Branch: `friendlies-0348`. Evidence: [preflight.log](friendlies-0348-runs/preflight.log).
The pre-existing untracked `imgui.ini` was left alone.

Read the supplied lead plan, `notes/doubles-v1-design.md`, and
`Tools/orca/doubles-rooms.py`. The existing build is Ninja, Release, with tests
and NoGUI enabled. The fixture uses isolated users and local dev tickets, but
requires an explicit `--disc`; it has no default disc. Filename discovery in the
worktree found no ISO, WBFS, RVZ, or DOL. The brief supplies no disc path.

Task hard rule 8 requires stopping on a block. No search of personal folders,
other worktrees, installed applications, or external services was attempted.
No game instances or fixture servers were started. No credentials were read.

## Requested gameplay cases

Every case below is **NOT RUN (blocked)**, not a runtime failure or a PASS.
There are no gameplay logs, confirmed hashes, desync counts, or fixture PASS
lines. The evidence for the shared prerequisite failure is
[preflight.log](friendlies-0348-runs/preflight.log).

| Case | Result | Remaining proof |
| --- | --- | --- |
| 1: three peers, two fight, one sits out | NOT RUN (blocked) | Confirmed hashes through fight and return to CSS; zero desyncs |
| 2: rotate at next CSS | NOT RUN (blocked) | Peer 1 sits out, peer 2 fights; second fight stays synchronized |
| 3a: four peers, two sit out | NOT RUN (blocked) | Two fighters and two idle ports through fight and CSS |
| 3b: all four play 2v2 | NOT RUN (blocked) | All four fighters, synchronized fight and CSS |
| 4a: sitting-out peer leaves permanently | NOT RUN (blocked) | Only that port drops; fighters continue |
| 4b: sitting-out peer away/back within budget | NOT RUN (blocked) | Retained seat returns; fighters retain their session |
| 4c: sitting-out peer ORCA_TEST_BLACKOUT | NOT RUN (blocked) | Existing rollback stall followed by recovery |
| 5: third friend arrives mid-fight | NOT RUN (blocked) | Hold notice, admission at next menu, unaffected fighters |
| 6: third friend arrives during another friend's recovery | NOT RUN (blocked) | Existing friend retains seat; no seated player is refused |
| 7: fifth arrival | NOT RUN (blocked) | Only fifth refused; four-player room continues |
| 8: ordinary room, third arrival | NOT RUN (blocked) | Only third refused with room_full; existing pair continues |

## Gates and changes

| Gate | Result |
| --- | --- |
| Required base ancestry | PASS: BASE-OK |
| Build tests target in build-orca-0343 | NOT RUN: stopped at prerequisite block |
| Full --gtest_filter=Orca* | NOT RUN: 0 tests executed |
| Doubles fixture self-test | NOT RUN: stopped at prerequisite block |
| Required gameplay cases | NOT RUN: no available disc |

No runtime fixes or regression tests were added: no runtime failure was observed.
Only this verification record and its preflight evidence changed. Compatibility,
two-player behavior, ranked behavior, and replay encoding were not changed.
No builds, installs, pushes, releases, production operations, or mailbox calls ran.

## Resume prerequisites and open questions

An authorized Brawl/P+ game image must be made available within this worktree
(or Luis must explicitly authorize a particular external game-image path).
Then build and execute the requested gates under the shared lock after checking
one-minute load below 18, with at most four headless instances and exact-PID
cleanup.

The fixture also needs extension before it can claim these proofs: its runner
currently waits for four reports even with three local roles, uses a single
input script for all roles, assumes one common initial plug frame, and has no
automated fight/rotation assertions. These are static observations, not proven
gameplay defects. Resume with per-role steering, scheduled arrivals/faults, and
scene/confirmed-hash assertions; do not add bench state or a rotation protocol.
