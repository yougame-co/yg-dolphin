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

## fix-0355: per-player recovery holds

Required pre-edit base check:

```text
789c8342f3 Handle recovery rejoin room closes
BASE-OK
```

Self recovery and peer recovery now retain independent deadlines. Self `recovering`
and rejoin WELCOME reconcile only the returning player's deadline. Peer `away`
reconciles only that peer's deadline, including a fresh announcement after rejoin.
The overlay uses self while reconnecting and the peer while waiting for that peer;
retry cutoff, input acceptance, session exemption and peer expiry use the matching
player's hold. Missing/invalid `hold_ms` retains the local 90-second fallback.

Updated rooms contract (supersedes the shared-deadline proposal above):

```text
self rejoin WELCOME hold_ms = the returning player's own remaining hold; a still-away peer is reported with its own away message + hold_ms.
```

Fake-clock regression coverage includes both crossed hold orderings at t=0/t=85/
t=89, a 1-second self WELCOME remainder, independent peer expiry and acceptance
of the server verdict, plus a fresh peer away deadline following WELCOME. Existing
single-player drop, short authoritative hold and fallback cases remain covered.
A missed peer away announcement gets an independent local fallback until its
own away message arrives; an authoritative present roster cancels a missed back.
While self is away, expiry follows the self deadline. The loopback fixture records
WELCOME's self remainder and still-away roster; `crossed-events.json` schedules
independent socket losses and a delayed peer away announcement after WELCOME.

Validation and final gates are recorded below after completion.

## fix-0355: ranked receipt reconciliation

Final acknowledgement removes the final and retained starts for that match only.
Unacknowledged game entries retain their original bytes and match identity and
continue retrying after the final. A retained authoritative ranked final waits
for those receipts before emitting its final score. The result panel accepts the
server outcome immediately and shows "Score pending" while receipts reconcile.
Late game receipts use the existing match/game dedupe, preserve game numbers,
and cannot send another finish after an authoritative final. A replayed start
cannot reset a pending final's tally. A settled set releases transport holds while
its receipt reconciliation finishes, then performs any deferred terminal ending.

Two fake-clock regressions cover a missing earlier receipt, later game plus final,
resend after final/start replay, unrelated/duplicate receipts, exact final tally,
and reconciliation when a final crosses a room hold and set-over. The gameplay
fixture's `--expect-ranked-reconciliation` withholds the first ranked game receipt
until the final has been delivered, serves the recorded receipt on the next report
retry, and checks exactly two game results plus one complete final on each peer.
`ranked-receipt-input.txt` uses the existing ranked stage/menu controls and repeated
walk-offs to produce two real game reports.

The first focused run passed 62/63 tests and exposed an existing runtime assertion:
a self rejoin must preserve a still-valid silent-input hold. The WELCOME cleanup
was corrected to retain it; an expired presence hold can retain an independent
input fallback after a missed peer back. The corrected build passes for both
`tests` and `dolphin-nogui`; both touched C++ translation units pass
`-Werror -fsyntax-only`, and the focused rerun passes **63/63, zero failures**.
The seven new fake-clock regressions and both public-WELCOME runtime cases are
included. Evidence: `build-recovery/fix-0355/{build,werror,focused}.log` and
`focused.xml`; the original failure is retained in
`focused-before-input-retention.{log,xml}`. Lock/load history is retained in
`build-recovery/fix-0355-gates.log` and `fix-0355-gates-retry.log`.

The initial ranked-expiry gameplay run exposed an unsettled-game ordering: a
local game report was submitted while the opponent's socket was absent, then
the server forfeited the set without an agreed game verdict. The authoritative
outcome must appear immediately while that game remains retryable. The panel
now shows the server outcome/rating with "Score pending" and emits no incomplete
final score; receipt reconciliation still publishes the complete final exactly
once. A pure panel regression covers pending and completed scores (including
zero wins) for both ordinary and forfeit verdicts. The forfeit verifier requires
that actual server outcome in the UX log, continued identical game-report retries,
and no falsely completed final score when the game's receipt is still unsettled.
The initial attempt is retained at `ranked-before-pending-score{.log,/}`.
The final gate rerun includes all touched UI translation units with `-Werror`.

### Final source and gameplay gates

Final build `tests` + `dolphin-nogui`: PASS. Final strict syntax checks: PASS for
all four touched C++ translation units (`YouGameRoom.cpp`, `SetEnd.cpp`,
`RoomTest.cpp`, `SetEndTest.cpp`), with no diagnostics. The existing duplicate
library/deployment-target linker warnings remain as previously documented.
Final focused recovery/resilience/public-WELCOME/result-panel suite: **87/87 PASS**.
The post-final input-silence regression uses a fresh timestamp to verify that a
settled set cannot restart a transport hold while receipts reconcile. Its test
binary was rebuilt, warning-checked, and rerun: **87/87 PASS (0.943 s)**.
Evidence: `build-recovery/fix-0355/{build,werror,focused}.log`,
`tests-refresh-{build,werror,focused}.log`, and the adjacent focused XML files.
Fixture self-test: PASS on final Python source.

All five existing gameplay runs and both new runs pass on the final engine:

| Run | Result / retained evidence in `build-recovery/fix-0355/` |
| --- | --- |
| `casual` | PASS: 16-second outage, same-seat return, confirmed hashes through frame 11,580; zero desyncs. |
| `legacy` | PASS: expected legacy network sentence without recovery; 33 matching pre-ending samples through frame 5,160; zero desyncs. |
| `ranked` | PASS: 96-second outage, absent player's network expiry, authoritative forfeit displayed with score pending while an unsettled game report continues retrying; 99 matching samples through frame 9,120; zero desyncs. |
| `outbox` | PASS: two lost receipt broadcasts, identical report replay, original seat, exactly one result per peer; 51 matching samples through frame 6,480; zero desyncs. |
| `deadline` | PASS: 3-second authoritative hold and verdict two seconds after it; 99 matching samples through frame 9,120; zero desyncs. |
| `crossed` | PASS: self returns after 85.987 seconds with only 4,012 ms on its WELCOME, while the away peer still has 83,950 ms. Delayed peer away refresh and authoritative forfeit accepted without early departure; 99 matching samples through frame 9,120; zero desyncs. |
| `ranked-receipt` | PASS: the earlier game receipt follows the later game and final; exactly two game results and one complete final per peer, scores `[2,0]` / `[0,2]`; 329 matching samples through frame 22,920; zero desyncs. |

Each row has its named `.log`, retained peer logs/stdout and `diagnostics.json`.
The two new gameplay fixtures each ran once. Fake-clock cases cover the exact
0/85/89-second orderings; the gameplay crossed ordering includes normal reconnect
backoff. The returning player's 4,012 ms never becomes the peer's hold deadline.

Reproduce the new runs with the common headless loopback command and authorized
disc documented above, room `recovery355`, scratch `build-recovery`, and controls
under `Tools/orca/fixtures/recovery-0355/`:

- Crossed: `--queue ranked --seconds 330 --input <controls>/ranked-input.txt
  --events <controls>/crossed-events.json --hold-ms 90000 --verdict-delay 2
  --expect-queue-ending forfeit --expect-crossed-holds`.
- Lost ranked receipt: `--queue ranked --seconds 330
  --input <controls>/ranked-receipt-input.txt --expect-ranked-reconciliation`.

Exact commands, load readings and exit statuses are retained by
`build-recovery/fix-0355-gates.py`, `fix-0355-gates-final.log`, and
`fix-0355-refresh-tests.{py,log}`. Every heavy command is wrapped separately in
the shared flock with nice 10 and the requested CCACHE_DIR. Build workers are six;
syntax checks are sequential and each test suite uses one process. Load is checked
before each job, with 60-second waits above 20 until below 18. All Dolphin runs use
two headless/Null peers, isolated users, ORCA_NO_LOCAL_INPUT=1 and the six SDL
variables. Only the supplied read-only disc path is used, without copying it.

Full final binary: **PASS, 2,280 tests, 2,257 passed, 23 existing
optional skips, zero failures/errors (608.794 s)**. Evidence:
`build-recovery/fix-0355/tests.log` and `tests.xml`. The existing loopback wrapper
supplies a minimal environment without live-service or disc-test opt-ins.

```text
CCACHE_DIR=/Volumes/YGWorktrees/ccache flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 python3 Tools/orca/room-fix-tests.py build-recovery/Binaries/Tests/tests --gtest_brief=1 --gtest_output=xml:build-recovery/fix-0355/tests.xml
```

`fix-0355-gates-final.log` ends with `ALL GATES PASS`; the separate refreshed-test
driver ends with `TEST REFRESH PASS`. Final diff whitespace check: PASS.
Generated `imgui.ini` is retained as `build-recovery/fix-0355/imgui-tests.ini`.
All requested local gates pass. The rooms team owns alignment with the updated
per-player contract above, and the release team owns release-kit/shipping work.
No local gate remains blocked. Local commits only; no version change, dependency
install, external-service request, push, release, signing or deployment.

## stalehold-0355: rejoin roster reconciliation

Required initial checks, before edits:

```text
35b5ed27da YG Dolphin 0.3.55
BASE-OK
```

`git merge-base --is-ancestor 35b5ed27da HEAD` passed on `recovery-0355`;
the starting worktree was clean. The change is confined to `YouGameRoom.cpp`,
`RoomTest.cpp` and this appended verification record.

A self rejoin WELCOME replaces every known still-away peer's previous public
deadline with receipt-time now plus its own `players[]` entry's valid `hold_ms`,
or a fresh local 90-second fallback. The public/input deadlines and host mirrors
are updated together. A later peer `away` reconciles its own remaining hold again.
Peers absent from the away roster have their holds cleared; subsequent input
silence can establish a new hold. A completed-match roster retains its pending
server verdict separately, preserving the existing final-verdict regression.
Friends and non-opted public rooms retain their existing policy.

Updated rooms contract:

```text
on a rejoin WELCOME, each still-away peer may carry its own remaining hold_ms; without it the engine waits the local 90 s for that peer until the peer's own away arrives.
```

The optional field belongs to that away peer's object in WELCOME `players[]`.
Top-level WELCOME `hold_ms` remains the returning player's own remaining hold.
Zero is valid; malformed/missing per-peer values use the independent fallback.

The new fake-clock regression records the peer's first away at t=0, self loss at
t=85, missed peer back at t=86 and second away at t=110, then WELCOME at t=111.
The t=111.003 tick stays open; input silence uses the refreshed peer deadline;
the peer's subsequent `hold_ms=2990` sets t=114; the server verdict at t=116 is
accepted. Fourteen combinations cover present/missing self remaining hold and
missing, valid, zero and malformed per-peer remaining holds. Both original B1
crossed orderings still assert their independent server deadlines, now supplied
in the corresponding peer's WELCOME entry. The existing pre-expiry rejoin test
also checks a fresh fallback, and present-roster tests check hold cancellation.

Build `tests` + `dolphin-nogui`: PASS, two build workers. Both touched C++
translation units pass `-Werror -fsyntax-only` without diagnostics. Existing
duplicate-library and dependency deployment-target linker warnings remain.
Focused recovery/resilience/public-WELCOME/result-panel suite: **88/88 PASS**.
Fixture self-test: PASS. Evidence is under `build-recovery/stalehold-0355/`;
`gates.py` records the commands and `gates.log` records load and exit status.

Intermediate attempts are retained there: the initial test compile needed an
explicit JSON number conversion; the first focused run needed its fixture's
relative `Tools` path available from the artifact directory; the next focused
run exposed the completed-match verdict retention described above. The corrected
build, warning checks and all focused cases pass.

The first casual gameplay attempt missed character selection before any outage:
port 2 attached at frame 3469, after its scripted one-shot selection. The failed
attempt is retained in `casual-before-control-delay{.log,/}`. Private copies of
the casual/ranked controls under the same artifact directory shift only character
selection commands by 600 frames (10 seconds), allowing seat attachment. The
checked-in controls, event files, observation durations and verifier are unchanged.
Final fixture commands in `gates.py` use those private input copies and the original
events under `Tools/orca/fixtures/recovery-0355/`.

All four required gameplay runs PASS on the final engine:

| Run | Result / evidence under `build-recovery/stalehold-0355/` |
| --- | --- |
| `casual` | PASS: 16-second outage, same-seat return after 17.649 s; 123 common confirmed samples through frame 11,580; zero desyncs. |
| `legacy` | PASS: expected no-opt-in network ending, no rejoin; 43 matching pre-ending samples through frame 5,760; zero desyncs. |
| `ranked` | PASS: 96-second outage, absent peer network expiry and authoritative forfeit accepted with pending score/report retries; 109 matching samples through frame 9,720; zero desyncs. |
| `crossed` | PASS: self returns after 83.872 s with 6,128 ms on WELCOME while its away peer has about 86,109 ms; delayed peer update and authoritative forfeit accepted; 109 matching samples through frame 9,720; zero desyncs. |

Each row has its named `.log`, retained peer logs/stdout and `diagnostics.json`.
All heavy commands are wrapped separately in
`flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10`, with
`CCACHE_DIR=/Volumes/YGWorktrees/ccache`. Build workers are two, strict syntax
checks are sequential, and unit tests use one process. Uptime is checked before
each job, waiting at 60-second intervals above 20 until below 18, with a 45-minute
load-wait limit. Every submitted job started below load 20. Gameplay uses two
headless/Null peers, isolated users, ORCA_NO_LOCAL_INPUT=1 and all six SDL disabling
variables. Only the supplied read-only Brawl path is used, without copying it.

Full final binary: **PASS, 2,281 tests from 183 suites; 2,258 passed, 23 existing
optional skips, zero failures/errors (613.026 s)**. Evidence: `tests.log` and
`tests.xml` under `build-recovery/stalehold-0355/`. The existing loopback wrapper
uses its minimal environment without live-service/disc opt-ins. Tests run from
the artifact directory (with a local `Tools` symlink for the wrapper), so generated
`imgui.ini` stays there. `gates.log` ends with `ALL GATES PASS`.

Final `git diff --check`: PASS. All requested gates pass; no local work remains
blocked. One local commit only; no version change, dependency install, external
service request, push, PR, release, signing or deployment.


## inputhold-0355: pending input recovery on rejoin

Required pre-edit checks:

```text
dc068dddcd Refresh public recovery state on rejoin
BASE-OK
```

`git merge-base --is-ancestor dc068dddcd HEAD` passed on `recovery-0355`;
the starting worktree was clean. Changes are confined to `YouGameRoom.cpp`, its
test trace snapshot in `YouGameRoom.h`, `RoomTest.cpp`, and this appended record.

This supersedes the preceding section's cancellation of a present peer's input
hold. A present, non-away peer on self rejoin clears stale presence state but
retains pending input recovery. An expired or missing peer deadline is restored
to receipt-time now plus the local 90 seconds; public/input deadlines and host
mirrors are reconciled under the mutex. A live retained deadline is preserved.
Input silence first observed while self is offline is remembered for rejoin.
The caller keeps its original continuous-stall timestamp. Contiguous input
receipt clears pending recovery and the host countdown; authoritative verdicts
and real departures still release it. Friends and non-opted public policy is
unchanged.

Fake-clock coverage includes peer away/input stall at t=0, self loss at t=85,
missed peer back at t=86, and present-peer WELCOME at t=111, with valid and
missing self `hold_ms`, using the original t=0 silence timestamp. It also covers
a still-live retained deadline, a missing deadline for a stall observed offline,
server verdict/departure, and the same sequence without recovery opt-in. The
existing public-WELCOME runtime case now keeps one real session across rejoin,
uses a silence timestamp older than 90 seconds, waits past its local timeout,
and resumes on contiguous inputs, checking recovery/countdown release.

Validation artifacts and exact commands: `build-recovery/inputhold-0355/`.
The gate driver checks uptime every 60 seconds and waits until one-minute load
is below 16 before each command, including a second check after acquiring the
shared build lock; load waits are capped at 45 minutes. Each heavy command uses
`flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10`, with
`CCACHE_DIR=/Volumes/YGWorktrees/ccache`, two build workers and one test process.
Gameplay commands reuse the preceding run's private delayed-selection input
controls and the checked-in events. They run two headless/Null peers at a time,
with isolated users, ORCA_NO_LOCAL_INPUT=1 and all six SDL disabling variables.
Only the supplied read-only Brawl path is used, without copying it.

Validation is **BLOCKED / NOT RUN** on this attempt. The driver exhausted its
45-minute load-wait allowance before submitting the build: 46 recorded
one-minute load checks ranged from 22.08 to 49.62, all above
the required <16 start threshold. The final check was 32.68. No build
lock job was submitted; the runner exited and no owned build/test/fixture process
remains. See `build-recovery/inputhold-0355/gates.log`.

Counts for this attempt: **0 unit tests and 0 gameplay fixtures run**. The
`tests` + `dolphin-nogui` build, both touched translation units' `-Werror` checks,
focused recovery/resilience/public-WELCOME/SetEnd tests, full test binary, fixture
self-test, and casual/legacy/ranked/crossed gameplay runs all remain outstanding.
The existing binaries have not been rebuilt for this change. Earlier PASS records
in this note belong to the preceding commits and do not validate this change.

Final source whitespace check: PASS. The requested single private local commit
records the implementation and regression coverage with validation explicitly
incomplete. No dependency install, external-service request, push, PR, release,
signing or deployment was performed.
