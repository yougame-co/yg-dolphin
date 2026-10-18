# recovery-0355 verification

Required initial base check (before edits):

```text
5097fe19d0 YG Dolphin 0.3.54
BASE-OK
```

Branch `recovery-0355`; local commits only. No version change, submodule initialization,
disc copy, dependency install, external service, push or deployment.

## Fix 1: match report outbox

Only opted-in public rooms retain the original encoded start/game/final reports.
Failed sends retry; unacknowledged game/final reports retry every two seconds, and
same-seat WELCOME replays the outbox without another gameplay callback. Retry uses
the retained bytes, without advancing report counters. Matching authoritative
receipts clear game/final entries; starts remain until the match ends. Existing
receipt deduplication still applies each result once. The outbox survives the
artificial-delay queue being discarded on reconnect. Legacy/friends sends retain
the existing policy. SendRaw now returns whether a write succeeded; delay queuing
is treated as pending delivery.

The fake transport can fail sends. `ReportsRetryOriginalBytesAndRejoinWithoutGameplay`
covers both queues, failed writes, successful writes with missing processing or
receipt, same-seat replay with no new callback, matching acknowledgements and
one local result. The existing ranked replay test now expects the required replay
of an unacknowledged game-start.

Build `tests` and `dolphin-nogui`: PASS. Focused recovery suite: 17/17 PASS.
`python3 Tools/orca/doubles-rooms.py --receipt-self-test`: PASS; loopback fake rooms
lose game/final receipts, retain them after match completion, and return the same
receipt for identical reports after rejoin. Logs: `build-recovery/recovery-0355-outbox-tests.log`
and `build-recovery/recovery-0355-receipt-fixture.log`.

## Web follow-up (review section 4)

This finding is web-only: terminal queue network status is mapped to rejoining
or generic network actions by web/src/lib/orca.ts. The web team should use queue
context and the actual terminal status sequence for both host and joiner, present
a terminal queue action, and preserve legacy network wording. No global engine
network-code or legacy wording change is made here.

## Rooms contract (pasteable proposal)

```text
For casual/ranked rooms opted in by WELCOME room.recovery:true:
- Add top-level hold_ms to messages announcing a hold: peer `away`, self
  `recovering`, and the self rejoin `welcome` (including a recovering WELCOME).
- hold_ms is a JSON integer >= 0, in milliseconds. It is the server's REMAINING
  hold at the instant that message is sent, not the original hold allowance,
  an absolute timestamp, or a fresh allowance for each reconnect.
- Send the current remaining value on every such announcement/rejoin. Zero
  means the server hold has reached its deadline; send the authoritative outcome.
- The engine reconciles one public deadline to receipt-time now + hold_ms.
  Without a valid hold_ms it retains today's local 90-second hold fallback.
- All public hold consumers use that deadline. Transport retries stop at the
  deadline. With a reachable room socket the engine continues reading for up
  to 10 seconds after it, accepting the authoritative result/finish (including
  forfeit or no-contest), set-over, or room-ended close. Without a verdict it
  ends locally as a network loss and never invents an outcome.
- Missing/false room.recovery leaves legacy public behavior unchanged; private
  rooms keep their existing policy.
```

## Fix 2: reconciled deadline and verdict grace

Input silence can create the fallback hold before rooms detects an outage. A later
server hold replaces that deadline and all public consumers read the shared value:
input holds, peer-away expiry, self retry cutoff, session timeout exemption,
returning input acceptance, and overlay countdown. Private deadline policy remains
unchanged. The overlay displays zero during verdict grace, retaining the original
hold deadline rather than extending its countdown by ten seconds. A reachable
socket may receive a verdict after retry cutoff; an absent socket still expires
at that cutoff. A false recovering flag on an initial WELCOME creates no hold.

New fake-clock coverage exercises the t=0 input silence / t=8 detection / t=90
old expiry / t=98 verdict ordering, 1.5-second remaining holds on peer-away and
self rejoin, verdicts two seconds after expiry, an absent verdict at the exact
10-second bound, self retry cutoff with a reachable socket, malformed/missing
hold_ms fallback, non-opted rooms ignoring the field, and ranked final report
retry/replay. The trace calls the production HoldInputSilence and RoomRecovering
methods. The old 90-second public peer expiry assertion now includes the requested
reachable-socket grace; disconnected self expiry stays at 90 seconds.

The fake rooms fixture now supports `--hold-ms`, `--verdict-delay`,
`--drop-report-receipts`, and `--expect-report-replay`. Away is sent immediately
without the previous 250 ms timing advantage. Ending runs verify every common
confirmed checksum sample instead of bypassing that comparison.

## Final gates

Final source build, warning checks, focused tests and all five gameplay runs PASS.
Final full binary: **2,270 tests, 2,247 passed, 23 existing optional skips, zero
failures (608.337 s)**. All requested gates PASS.
All heavy jobs use the shared flock and nice 10; build workers <= 6 and one test
process. The gate driver logs uptime before each job, waits above load 20 and
resumes below 18, with a 45-minute limit. Every Dolphin fixture uses headless/Null,
isolated users, ORCA_NO_LOCAL_INPUT=1 and all six SDL disabling variables, with
two peers at a time. Only the supplied read-only Brawl path is used.
Every heavy job started below load 20. Load rose above 20 during the
already-running final suite; no new heavy job was started during that interval.

Reproduce with the configured build's `Binaries/dolphin-emu-nogui`, the authorized
disc path, `--run --local --queue <queue> --room recovery355 --scratch build-recovery`,
and the following controls under `Tools/orca/fixtures/recovery-0355/`. Wrap each
command in `flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10` after uptime.

| Run | Queue / options | Input / events | Seconds |
| --- | --- | --- | --- |
| Existing casual recovery | casual | casual-input.txt / casual-events.json | 140 |
| Existing legacy ending | casual, `--no-recovery --expect-queue-ending network` | casual-input.txt / casual-events.json | 120 |
| Existing ranked expiry | ranked, `--expect-queue-ending forfeit` | ranked-input.txt / ranked-events.json | 240 |
| Report outbox | casual, `--drop-report-receipts 2 --expect-report-replay` | outbox-input.txt / outbox-events.json | 180 |
| Short server deadline / late verdict | ranked, `--hold-ms 3000 --verdict-delay 2 --expect-queue-ending forfeit` | ranked-input.txt / ranked-events.json | 180 |

Each run uses `--input`, `--events`, `--seconds`, and `--keep-logs` for those files,
duration and a separate artifact directory. The two-player controls wait for seat
attachment before character selection. Ranked also exercises the legal stage timeout
path before its actual game-start; its observation windows include that setup.


Final source evidence (`build-recovery/recovery-0355/`):

- `build-final.log`: PASS, tests and dolphin-nogui, six build workers.
- `werror-final.log`: PASS, all three touched C++ translation units
  (YouGameRoom.cpp, SetEnd.cpp, RoomTest.cpp; the resilience cases share RoomTest.cpp).
- `focused-final.log`: 54/54 PASS, including all 29 unchanged friends resilience
  cases and both public WELCOME runtime cases.
- `fixture-self-test-final.log`: PASS on final Python source; original private,
  version, public queue, receipt loss, completed-match receipt replay and forfeit checks.
- First full binary: 2,268 tests, 2,245 passed, 23 existing optional skips, zero
  failures (609.327 s). This preceded the final two added focused cases and final
  deadline cleanup. `build-recovery/recovery-0355-tests.log` and its adjacent XML.
- `tests-final.log` and `tests-final.xml`: final full binary PASS, **2,270 tests,
  2,247 passed, 23 existing optional skips, zero failures**, 608.337 s. No live
  service or disc-test opt-ins were supplied to the full binary.

| Final gameplay run | Result / evidence |
| --- | --- |
| casual-final | PASS: 16-second socket drop; same-seat return after 18.691 s; 122 common samples through frame 11,580; zero desyncs. |
| legacy-final | PASS: exact legacy network sentence, no rejoin; 33 common pre-ending samples through frame 5,160; zero desyncs. |
| ranked-final | PASS: 96-second socket drop; authoritative winning forfeit, absent peer network expiry, no local winning finish; 99 common pre-ending samples through frame 9,120; zero desyncs. |
| outbox-final | PASS: two deliberately lost receipt broadcasts; host sends three identical finals and joiner two; rejoining seat stays 1 -> 1; exactly one won/lost result per peer; 53 common samples through frame 6,540; zero desyncs. |
| deadline-final | PASS: self recovering and peer away carry a 3-second remaining hold; disconnected self ends at 3.001 s; reachable peer accepts server forfeit at 5.001 s, two seconds after expiry; 99 common samples through frame 9,120; zero desyncs. |

Each run's adjacent `.log` and retained peer logs/stdout/diagnostics directory use
that name. `gates.log` and `gates-resume.log` record locking/load/exit status; the
adjacent Python drivers record exact commands. The only intermediate fixture
failure was a verifier wording assertion in `outbox-before-verifier-fix`: a
completed match's opponent had already left, so the engine correctly returned
the same seat with `same-seat-fresh-drop-in` rather than a live-session `room-back`.
The retained attempt already shows identical retries and one result per peer.
The verifier now accepts that completed-match return while requiring the original
seat and outcome count; the full gameplay rerun PASS is recorded above. No engine
change was needed for that verifier correction.

All work is local. No engine work or gate remains blocked. Rooms must implement
the pasted remaining-hold contract; the web team owns the terminal queue mapping
described above. No deployment, push, release or version bump is included.

## adopt-0355: Windows window adoption (2026-10-18)

Initial checks on `recovery-0355`, before edits:

```text
70d919e283 Reconcile public room recovery deadlines
BASE-OK
```

`git merge-base --is-ancestor 5097fe19d0 HEAD` passed; the starting worktree was
clean. The code change is confined to `Source/Core/DolphinNoGUI/PlatformWin32.cpp`.
Both wait-loop and final adoption checks call the tiny `IsAdoptedBy(HWND, HWND)`
helper, which compares `GetAncestor(self, GA_PARENT)` to the requested parent.
The single named timeout is 20 seconds, allowing the desktop launcher's late
`SetParent` retry. Message pumping and the real-timeout error path, including
the existing diagnostic's literal "10 s" and status sentence, are unchanged.

After adoption, `SetWindowLongPtr(GWL_STYLE)` clears `WS_POPUP`, sets `WS_CHILD`
and preserves both clipping styles. `SetWindowPos` applies `SWP_FRAMECHANGED`
with no move, resize, z-order change or activation before the existing placement.
The placement helper also reads `GA_PARENT`. Focus and keyboard routing already
use `GA_ROOT` to find the foreground/focus target and need no changes.

There is no existing unit seam for the native adoption predicate; the helper is
kept local and directly reviewable. macOS cannot run Win32 window APIs or the
cross-process adoption scenario, and the configured compilation database has
zero `PlatformWin32.cpp` entries (CMake selects it only under `WIN32`). Thus no
runtime adoption test or native Windows `-Werror` compile can run on this host.
Diff review checked HWND/GA_PARENT usage, LONG_PTR style-mask width, style order,
SetWindowPos flags and preservation of the timeout path. Windows MSVC compilation
and adoption testing remain with the release kit. Non-Windows code/build files
are untouched.

Host build: PASS (exit 0), using:

```text
CCACHE_DIR=/Volumes/YGWorktrees/ccache flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 cmake --build build-recovery --target tests dolphin-nogui --parallel 6
```

`uptime` was checked below load 20 before submission; the command waited for the
shared lock. Log: `build-recovery/adopt-0355-build.log`. The build has 28 linker
warnings about duplicate libraries and existing Homebrew dylibs targeting newer
macOS versions than the configured deployment target; no C++ compiler warnings.
The changed Windows translation unit is excluded on macOS as expected.

Full test binary: PASS (exit 0), **2,270 tests from 183 suites; 2,247 passed,
23 existing optional skips, zero failures/errors (609.199 s)**. Command:

```text
CCACHE_DIR=/Volumes/YGWorktrees/ccache flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 python3 Tools/orca/room-fix-tests.py build-recovery/Binaries/Tests/tests --gtest_brief=1 --gtest_output=xml:build-recovery/adopt-0355-tests.xml
```

Load before submission was 13.38. The existing wrapper supplies loopback-only
fixtures and a minimal environment without live-service/disc opt-ins, and runs
one GoogleTest process. Evidence: `build-recovery/adopt-0355-tests.log` and
`build-recovery/adopt-0355-tests.xml`. The generated `imgui.ini` is retained as
`build-recovery/adopt-0355-imgui.ini` instead of leaving a root-level artifact.

Final `git diff --check`: PASS. Manual Windows API/diff review: PASS; native
Windows compilation, warning enforcement and adoption runtime verification
remain for the PC release kit as noted above. No host gate is blocked. Local
commit only; no push, release or deployment.


## close4004-0355: recovery rejoin refusal (2026-10-18)

Required initial checks on `recovery-0355`, before edits:

```text
85612596a8 Align Windows window adoption handling
BASE-OK
```

`git merge-base --is-ancestor 70d919e283 HEAD` passed. The starting worktree
was clean; work continues from that HEAD with no merge or rebase.

An undecided public room opted into recovery now handles close 4004 immediately,
before reconnect policy: it ends with outcome `network` and the exact sentence
"You couldn't rejoin the match (the time to reconnect ran out)." This covers
`rejoin-refused`, `rejoin-refused:allowance` and `rejoin-refused:not-your-seat`.
The existing transport exposes the close code without its reason text, so the
decision uses 4004 inside the existing public recovery gate. A prior authoritative
verdict wins, including a casual result and a result after cleared match metadata.
Friends, non-opted public rooms and disabled recovery retain their existing
4004 outcome and wording.

Unit coverage checks casual/ranked rooms, each refusal reason and the transport's
close diagnostic, both with and without an existing recovery hold. It checks
immediate termination, no retry now or after polling, cleared recovery state,
no invented result, legacy/friends behavior, and authoritative-verdict precedence.
The existing completed-WELCOME test also covers cleared match metadata.

Build `tests` and `dolphin-nogui`: PASS (exit 0), six build workers.
Both touched C++ translation units: PASS with `-Werror -fsyntax-only` (exit 0).
The build has the previously documented linker warnings about duplicate libraries
and dependency macOS deployment targets; the warning check has no diagnostics.
Evidence: `build-recovery/close4004-0355-build.log`,
`build-recovery/close4004-0355-werror.log` and its adjacent Python driver.

Commands, each submitted after checking uptime below load 20:

```text
CCACHE_DIR=/Volumes/YGWorktrees/ccache flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 cmake --build build-recovery --target tests dolphin-nogui --parallel 6
CCACHE_DIR=/Volumes/YGWorktrees/ccache flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 python3 build-recovery/close4004-0355-werror.py
```

Focused recovery/resilience tests: PASS, **54/54 tests, zero failures/errors**
(0.003 s). This includes the new refusal, legacy/friends and verdict precedence
regressions. Evidence: `build-recovery/close4004-0355-focused.log` and adjacent XML.

```text
CCACHE_DIR=/Volumes/YGWorktrees/ccache flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 python3 Tools/orca/room-fix-tests.py build-recovery/Binaries/Tests/tests '--gtest_filter=OrcaRoomRecovery.*:OrcaRoomResilience.*' --gtest_brief=1 --gtest_output=xml:build-recovery/close4004-0355-focused.xml
CCACHE_DIR=/Volumes/YGWorktrees/ccache flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 python3 Tools/orca/room-fix-tests.py build-recovery/Binaries/Tests/tests --gtest_brief=1 --gtest_output=xml:build-recovery/close4004-0355-tests.xml
```

Full test binary: PASS (exit 0), **2,272 tests from 183 suites; 2,249 passed,
23 existing optional skips, zero failures/errors (608.613 s)**. Evidence:
`build-recovery/close4004-0355-tests.log` and adjacent XML. The existing wrapper
uses loopback fixtures and a minimal environment without live-service or disc
opt-ins, with one GoogleTest process. Generated `imgui.ini` is retained at
`build-recovery/close4004-0355-imgui.ini`.

All heavy commands used the shared flock, nice 10 and the requested CCACHE_DIR;
load was below 20 before each submission. Build workers were capped at six,
warning checks were sequential, and tests used one process. Commands waited for
the shared lock as needed. No installs were needed.

Final `git diff --check`: PASS. All requested host gates passed; nothing remains
blocked. One local commit only; no push, release or deployment.
