# fix-0348 verification

Private local work on `train-0348`, 2026-10-16, reviewing `85112b1bd6`.
No push, release, service access or production operation is part of this task.

## Base

Before reading the report or changing files:

```text
6f54c6e5ec Complete Project+ 0.3.48 verification
BASE-OK
```

Command: `git log -1 --oneline && git merge-base --is-ancestor 6f54c6e5ec HEAD && echo BASE-OK`.
The worktree was clean. The socket-drop report was read first at the explicit path in the brief.

## Page compatibility audit

Read locally, without fetching:

- YouGame `origin/main`: `051b8629852cd8c23c102b202fd2a1a3b74bb93e`, `web/src/lib/orca.ts` ROOM_ERRORS.
- YouGame `origin/claude/v2`: `c70f2fd9bdeefd7000b98065ee985444aa2f97b1`, same file/set.

Both sets contain `room_mismatch`, `mismatch`, `room_full`, `kicked`, `network`,
`peer_left`, `desync`, `host_gone`, `signed_out`. Only v2 additionally contains
`host_older`, `host_newer`, `lobby_mismatch`. Status.h now documents the intersection.
The table audits all End codes in YouGameRoom.cpp and their reporting boundaries
(Online.cpp AppCode/ReportSessionEnd and OnlineMatch.cpp FailJoin/KeepRoomOpen).
“Known” refers to membership in ROOM_ERRORS, not another UI error table.

| Trigger / original internal or proposed code | Known to main page: yes/no | Known to v2 page: yes/no | Final emitted `orca error` code / treatment |
| --- | --- | --- | --- |
| Replay catch-up rejection `replay_limit` | no | no | `network`; keep the longer explanatory sentence |
| Exhausted initial held-seat refusal `seat-in-use` | no | no | `network`; plain held-seat sentence |
| 409 `seat_in_use` body code | no | no | Retry policy, then `network`; body tag is never a wire error code |
| 409 full or other non-seat conflict, `room_full` | yes | yes | `room_full` (queue retains status-only 409 policy) |
| 409 complete-match body / `match_complete` | no | no | Recognize real `This match is complete`; internal `match-over` below |
| Internal `match-over` (409 complete-match or close 4004) | no | no | Never emitted as an error; existing callers emit `host-left match-over` / `friend-left match-over` state events and reset the completed match |
| 403 `host_older` | no | yes | `room_mismatch`, version-specific sentence for friends |
| 403 `host_newer` | no | yes | `room_mismatch`, version-specific sentence for friends |
| 403 `lobby_mismatch`, absent/unknown body code | no | yes (lobby_mismatch) | `room_mismatch`, existing mismatch sentence |
| Other 403 refusal | yes (`room_mismatch`) | yes (`room_mismatch`) | `room_mismatch` |
| 401 `Bad or expired ticket` | yes (`network`) | yes (`network`) | `network`; expired-invite sentence; initial refusal makes one request |
| 410 `host_gone` | yes | yes | Existing `network` mapping; friends server-close recovery retries first |
| 503 | yes (`network`) | yes (`network`) | `network` after server-close refusal grace; ordinary transport recovery retains its hold |
| Close 1008 `message-limit` | no | no | `network` |
| Close 4003 `locked` | no | no | `network` |
| Successful-reconnect budget `reconnect-limit` | no | no | `network` |
| Replaced hosting socket / other host, internal `taken` | no | no | Existing Online.cpp AppCode maps to `network`; RoomTaken retains the re-host behavior |
| Close 4001 / kick message, `kicked` | yes | yes | `kicked` |
| Peer leave/hold expiry/input stall, `peer_left` | yes | yes | `peer_left` |
| Peer mismatch / wrong lobby, `room_mismatch` | yes | yes | `room_mismatch` |
| Queue match-configuration refusal, `mismatch` (OnlineMatch) | yes | yes | `mismatch`; unchanged |
| Desync drop, `desync` | yes | yes | `desync` |
| Keyframe sign-in drop, `signed_out` | yes | yes | `signed_out` |
| Ticket/bridge/seat identity/welcome/error-message failures and transport/hold failures, default `network` | yes | yes | `network`; arbitrary server text never becomes a code |
| Explicit user leave, empty code | n/a | n/a | No `orca error` |

No unknown code is emitted. `match-over` and `taken` remain private control signals
because changing them would alter completed-match and re-host behavior. Their existing
reporting boundaries consume/map them before any error line. Other unknown End codes
were changed at their source. The numeric HTTP statuses are never error codes themselves.

## Behavior and tests

- Initial friends-room seat refusal keeps the existing exponential backoff (500 ms first,
  then 1/2/4 s, capped at 4 s with 70–130% jitter), and cannot end for a persistent
  held seat before 12 seconds. At midpoint jitter its requests are at 0.5, 1.5, 3.5,
  7.5, 11.5 and 15.5 seconds; the final refusal at 15.5 s ends as `network` with:
  `Your seat in that room is still held by your last connection. Try again in a few seconds.`
  Fake-clock coverage pins that deadline and a successful welcome after the 10 s window.
- After a friends-room server close, 410, 409 `seat_in_use`, and 503 refusals retry
  for 10 seconds from the original loss. Retries never reset that origin. Backoff
  is clamped to an attempt at the grace boundary. Persistent refusals then end with
  existing refusal wording (the held-seat sentence for seat-in-use), using known codes.
  A returning seat keeps its cached ticket on 410/seat-in-use; 401 still allows the
  existing one-fresh-ticket fallback. Final notices retain the last refusal, including
  its HTTP status. Fake-clock tests cover retries, exact boundary,
  recovery inside the window, and immediate final 403/full-room controls.
- The first reconnect after receive failure or EOF is immediate. Later attempts still
  use jitter; silence/send failures retain the original initial delay. Ranked/casual
  rooms get no recovery window or new reconnect behavior.
- Friends 403 wording exactly follows the brief: older host adds ` (<host_version>)`
  immediately after `YG Dolphin` only for a validated version; it asks for Play and
  a new invite. Newer host says `Press Play to update, then join again.` Missing codes
  and lobby_mismatch retain the existing mismatch sentence. Queue refusal tags are
  stripped for 403 as for 409, and the fallback says `the other player`.
- Real refusal-body fixtures cover the pending older-host JSON with host 0.3.47 / self
  0.3.48, optional version absence, newer host, lobby_mismatch, 401, room full, match
  complete, seat_in_use, and host_gone. The locally inspected v2 newer-host body says
  `Press Play to update yours.`; the client intentionally renders the wording in the brief.
- BestEffortLeave now always reserves a 200 ms Close(1000) budget even if its ticket /
  connect / leave work exhausted the earlier two-second deadline. It returns as soon
  as the acknowledgement or socket end arrives. The close send retries CURLE_AGAIN
  within that same budget and handles partial successful writes. The server-close
  echo shares that send helper while retaining its 250 ms bound. No fixed sleep was added. This executes on the room I/O
  thread; Online::ShutdownInBackground retains the asynchronous emulation-thread path.
  Loopback tests cover a 60 ms close acknowledgement and one delayed beyond the budget.
- Existing echoing of server-initiated closes is retained, including empty close frames;
  0.3.44 already sent close frames for its own closes. No change claims otherwise.

## Exact diagnostics format

Existing prefixes and fields remain first. Optional fields are appended:

```text
orca state room-away self <kept|retry|ended> <reason> attempt=<n> close-code=<code> close-reason=<text> last-error=<text> <recv|eof|silent|send> [errno=<CURLINFO_OS_ERRNO>] [colo=<old>-><new>]
```

The log copy retains `Orca room: room-away self ...`. `room-back self ...` uses the
same context suffix. Final non-retry closes retain their shorter existing
`room-away self ended <reason> close-code=<code> close-reason=<text>` prefix and append
available diagnostics in the same order. All earlier fields are untouched so the
page's cleanOrcaRoomLine can still parse them. A loss token is omitted when none is
available (e.g. a test drop/initial refusal). `silent` is the local silence timeout;
`send` is a failed send; `recv` is a receive error; `eof` includes stream end and a
server close frame. `errno` is omitted unless curl returned the OS-error info (zero
is a valid returned value). Before a new socket is known, `colo=<old>`; with only a
new colo, `colo=<new>`; with both, `colo=<old>-><new>`. No colo field without either.
Only the validated three-uppercase-letter CF-RAY suffix is retained. No full CF-RAY
identifier or arbitrary header value enters logs. Loopback tests cover absent and
invalid headers, mixed-case header name, metadata retained after cleanup, and an
EWR->IAD reconnect with loss/errno and the unchanged prefix.

## curl patch and portability

`Externals/curl/CMakeLists.txt` from 85112b1bd6 is already platform-independent and
requires no Mac-only correction. Exactly what it does:

1. After adding curl, read vendored `curl/lib/http.c` and register it with
   CMAKE_CONFIGURE_DEPENDS. Fail configure if the expected WebSocket guard is absent.
2. In a generated build-directory `orca-http.c`, replace
   `if(data->req.upgr101 == UPGR101_WS) {` with
   `if(data->req.upgr101 == UPGR101_WS && k->httpcode < 400) {`.
3. Remove the original http.c from the compiling target and add the generated copy.
   Select `libcurl_object` if present (curl's default shared-object arrangement on
   Windows), otherwise `libcurl_static` (the usual Linux/macOS static arrangement).
4. Add the vendored curl/lib directory to that target's private include path so
   the relocated source finds its internal headers. Existing target definitions,
   platform headers, generated curl config and link libraries remain in force.

Curl otherwise aborts a refused WebSocket upgrade at the headers, discarding the
body. The generated guard allows HTTP >=400 refusal bodies to reach the existing
8 KiB bounded callback on the original request; WebSocket::Connect still explicitly
requires HTTP 101 before using the WebSocket API. HTTP 2xx/3xx upgrades still fail.
No second probe request, submodule edit, platform-specific source path, shell patcher
or compiler-specific flag is introduced. The source extension remains `.c` for MSVC
and Linux C compilers. Windows still uses Schannel, Unix still uses mbedTLS; the
Winsock / POSIX wait code remains conditional as before. Native MSVC proof belongs
to orca-lead on the PC; this Mac run does not claim a native Windows or Linux build.

## Gates

All local gates passed on the final sources. The full binary selected 2,192 tests
across 172 suites and ran in 609.001 seconds.

| Gate | Result | Evidence under `build-orca-tests/` |
| --- | --- | --- |
| Build, `cmake --build build-orca-tests --target tests -j2` | PASS | `fix-0348-build.log`, `fix-0348-final-build.log` |
| Focused refusal/recovery/close/logging checks | 42 passed, 0 failures | `fix-0348-focused.log`, `fix-0348-focused.xml` |
| Final real compile commands plus `-Werror` | 5/5 object compiles passed | `fix-0348-werror.log` |
| Full binary, including all focused cases on final sources | 2,169 passed, 23 skipped, 0 failures/errors | `fix-0348-tests.log`, `fix-0348-tests.xml` |
| `git diff --check` | PASS | Clean output |

The existing 23 optional skips are live-service/bridge opt-ins, disc-dependent
checks, two bridge-latency checks and one design export. No live test was enabled.
The full suite includes replay/hash controls, ordinary two-player rooms, ranked,
casual, doubles, long stall budgets and the 90-second recovery/hold controls.

All builds and test runs used the shared lock and nice level 10, with two build
jobs and one test worker. `uptime` was checked before each gate. The first build
waited 60 seconds for load to fall from 21.60 to 16.40; later gates started below
18 (final full-suite check: 10.24). Compiler-cache writes were disabled. The runner
uses a minimal test environment, loopback fixtures, ORCA_NO_LOCAL_INPUT and the
six SDL settings already used by the preceding room-close verification. No installs,
live service calls, game instances, pushes, releases or changes outside this
worktree and the explicitly allowed lock/temp paths were performed.

Reproduction (the load-check wrapper rechecks uptime every 60 seconds and stops
only after 45 minutes of continuous high load):

```sh
python3 build-orca-tests/fix-0348-gate.py cmake --build build-orca-tests --target tests -j2
python3 build-orca-tests/fix-0348-gate.py python3 build-orca-tests/fix-0348-werror.py
python3 build-orca-tests/fix-0348-gate.py python3 Tools/orca/room-fix-tests.py \
  build-orca-tests/Binaries/Tests/tests \
  --gtest_output=xml:build-orca-tests/fix-0348-tests.xml
```

Each wrapper runs exactly its one command under
`flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10` and releases the lock
between gates. The -Werror reproducer uses this tree's compile_commands.json,
removes dependency-output options, writes separate object files, and compiles
WebSocket.cpp, YouGameRoom.cpp, OnlineMatch.cpp, RoomTest.cpp and generated
orca-http.c. Touched headers are checked through those includes.

Remaining local work: none. Blocked: none. Native MSVC proof remains with orca-lead
on the PC, as requested. Linux portability was reviewed at the source/CMake level;
a native Linux build was not run on this Mac. Nothing was staged or deployed to a service.
