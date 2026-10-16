# Finish 0.3.48 verification

Task `finish-0348`, 2026-10-16, branch `train-0348`. **Complete: all required
gates PASS.** Local commits only.
No push, external service, production action, signing or MCP call was made.

## Required base checks

The original pass checked `85112b1bd6` before edits. On this resumed pass,
before any changes:

```text
56e1109463 Update 0.3.48 notes and replay sample checks
BASE-OK
```

`git merge-base --is-ancestor 56e1109463 HEAD` succeeded; the worktree was clean.
That required base contains the original notes, replay pin and Project+ fixture
changes. The earlier machine-load interruption is resolved by the runs below.

## Changes

- ORCA.md 0.3.48 player notes describe sitting out and the three player-visible
  changes from `85112b1bd6`. The four-player-room announcement remains under
  its original 0.3.46 release.
- Current compatibility text states engine 31, Brawl/Project+ game revision 1
  from 0.3.48, results-only/GenericModule revision 1 and legacy launcher 0.
  Other `ux=` references were checked; older numbered entries describe history.
- Friendlies case 4c plainly proves seat retention after the fixture closes a
  socket whose send blocked for one second, followed by reconnect.
- The replay golden asserts that `Stream(Replay(30, 0), 10)` is 1,516 bytes with
  SHA-256 `36d2a4e821fd27739e20fcd4726b42be65d1c3420b2e7a47bc47ea79f2222c7e`.
  Its comment requires updating the YouGame rooms `streams.chunked` fixture too.
- The fixture accepts the legacy `--pplus` launcher and disables local input
  on every peer. Its Project+ controls shorten the CSS cursor move and let
  only the host select a stage. No further input changes were needed on resume.
- The load gate now waits, printing `uptime` every 60 seconds, until the
  one-minute load is below 18; it gives up only after 45 continuous minutes.
  It rechecks after acquiring the lock. Both resumed live runs completed
  without a load interruption. The former load stop is retained as an
  incomplete attempt, never claimed as a PASS.

## Results

| Check | Result | Evidence |
| --- | --- | --- |
| Project+ case 2, three peers and two fights | PASS: humans 1/2 then 1/3; outgoing port 2 becomes empty; both fights return to CSS; matching hashes through 9780; 0 desyncs | [fixture](friendlies-0348-runs/pplus-case-2-attempt-3/run.log), [all-peer probes](friendlies-0348-runs/pplus-case-2-attempt-3/evidence-summary.json) |
| Project+ case 1, separate three-peer run | PASS: humans 1/2, port 3 sits out; fight returns to CSS; matching hashes through 9780; 0 desyncs | [fixture](friendlies-0348-runs/pplus-case-1-attempt-1/run.log), [all-peer probes](friendlies-0348-runs/pplus-case-1-attempt-1/evidence-summary.json) |
| Final tests + NoGUI incremental build, two jobs | PASS, including the final `<string_view>` include | [build](finish-0348-build.log) |
| Touched C++ actual object compile with `-Werror` | PASS: `KeyframeTest.cpp` with its real compile flags | [compiler](finish-0348-werror.log), [reproducer](finish-0348-werror.py) |
| Python runner regressions with `-Werror` | PASS: 4/4, including the new legacy Project+ command/environment check | [regressions](finish-0348-fixture-regressions.log) |
| Doubles fixture self-test with `-Werror` | PASS | [self-test](finish-0348-doubles-selftest.log) |
| Final full test binary, loopback fixtures and authorized disc | PASS: 2,165 passed / 21 guarded skips / 0 failures; 2,186 tests in 172 suites | [full log](finish-0348-full-tests.log), [XML](finish-0348-full-tests.xml) |
| `git diff --check` | PASS | |

The full binary runs without a filter through the existing room/keyframe
loopback wrapper. The wrapper supplies a minimal environment, all six SDL
variables and `ORCA_NO_LOCAL_INPUT=1`, without live-service opt-ins.
The 21 guarded skips are 14 live-service opt-ins, one live bridge/room check,
two unavailable Rev 1-disc checks, two bridge-latency checks, one design-only
export, and one Rev 2 check that also requires the default macOS Sys resource
layout. They are skips, not passes. The Rev 2 read-patch and font checks pass.
The XML replay property independently confirms the pinned 1,516-byte SHA-256
sample above. Retained text logs have only trailing whitespace removed;
the binary probes and XML are preserved byte for byte.

See [Project+ details](friendlies-0348-verification.md#project) for per-role
controls, all retained attempts and reproduction. Each resumed run used a
fresh writable APFS launcher/SD clone, three headless Null-video, muted-audio
instances, and the authorized read-only Brawl Rev 2 disc. Only exact owned
child PIDs were stopped. Both clones were deleted; the shared source and
disc were never written.

Reproduce the final build/test gates with:

```sh
python3 notes/finish-0348-gates.py
```

The orchestrator wraps each heavy command separately in
`flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10`, uses two build
jobs and one test worker, prints `uptime` before each job, checks the load
ceiling under the lock, and disables ccache writes. Build outputs stay in
`build-orca-tests`; isolated runtime/test users stay in this process's system
temporary directories.

Each heavy job started below load 18. During the already-running full binary,
two `uptime` samples reached 18.29 and 18.74; no additional heavy command
was started. The respective requested 60-second rechecks were 16.97 and 14.58,
below 18. The existing test run continued and completed with 0 failures.

No required work remains incomplete or blocked. The local completion commit
records these notes, the load-wait adjustment and the evidence; no push.
