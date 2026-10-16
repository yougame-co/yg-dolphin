# room-close-0348 verification

Private local work on `train-0348`, 2026-10-16. Build directory: `build-orca-tests`.

## Base

Before any edits:

```text
$ git log -1 --oneline
9ee6dec6cf Merge ckt-generic-compat-0348 into train-0348
$ git merge-base --is-ancestor 9ee6dec6cf HEAD && echo BASE-OK
BASE-OK
```

The worktree was clean. The recent log included the existing train-0348 work,
`57646f131e` (elf-launcher-hash merge), and `9ee6dec6cf` (ckt-generic-compat merge).

## Changes and coverage

- Server closes: reply with the received status code and no reason before marking the
  socket closed and releasing it. Empty closes receive an empty reply. The reply has a
  bounded 250 ms write wait. `Close` still sends only while open. `StartReconnect`
  already called `Close(4001)` at this base; the new runtime test verifies that frame.
- Reconnect admission: classify the real 409 `seat_in_use` JSON body, use the cached
  ticket first, and try one fresh ticket only after another HTTP 4xx refusal. A transport
  error or seat-in-use keeps the cached ticket and existing retry policy/deadline.
  Loopback tests record ticket sequence numbers, check cached reuse, fresh fallback,
  initial and recovery seat-in-use retries, and a final genuine room-full refusal.
- Re-hosting: `KeepRoomOpen` clears `home_code` for `room_full`, just as for `RoomTaken`,
  so its next delayed open requests a new code. The source change is confined to that
  existing condition; its recovery timing is unchanged.
- Joiner wording: a host's `replay_limit` drop now explains that the match is too long
  to catch up and suggests trying at the next character select. The host already
  forwards that reason; the runtime test sends it through the real room message path.
- 403 bodies: parse `host_older` and `host_newer`, include a valid optional numeric
  `host_version` for the older-host sentence, and retain the existing mismatch wording
  for `lobby_mismatch`, unknown codes and absent codes. Tests exercise the complete
  JSON bodies, optional fields and typed field handling. The site-facing error remains
  `room_mismatch`. The existing `Status::Report(code, sentence)` protocol has no separate
  end-report reason field, so no report schema change was made.

The exact seat-in-use and room-full sentences were read using the requested local
`git -C /Users/luis/Code/YouGame show origin/main:rooms/src/room-rules.ts` and
`...:rooms/src/index.ts` commands. The 403 tests use the supplied code/optional-version
schema and the Worker's lobby-configuration sentence as `error`; classification reads
`code`, not that sentence.

The bundled curl aborts a refused WebSocket upgrade at the headers before its existing
body callback runs. `Externals/curl/CMakeLists.txt` builds a generated local copy of
`http.c` that lets HTTP errors reach the bounded body callback on the original request.
`WebSocket::Connect` explicitly requires status 101. The vendored submodule is unchanged;
no second probe request is used. Both static and object curl targets are handled. A
changed upstream condition requires review at configure time.

No gameplay, compatibility-key, ranked policy, packet or replay serialization code changed.

## Gates

All required local gates passed. The full suite selected 2,186 tests across 172 suites
and completed in 600.971 seconds.

| Gate | Result | Evidence under `build-orca-tests/` |
| --- | --- | --- |
| `cmake --build build-orca-tests --target tests -j2` | PASS | `room-close-0348-build.log` |
| Focused runtime regressions and cancellation/initial-refusal controls | 10 passed, 0 failures | `room-close-0348-focused.log`, `room-close-0348-focused.xml` |
| Real compile commands plus `-Werror` | 5/5 object compiles passed | `room-close-0348-werror.log` |
| Full binary through `room-fix-tests.py` | 2,163 passed, 23 skipped, 0 failures | `room-close-0348-tests.log`, `room-close-0348-tests.xml` |
| `git diff --check` | PASS | Clean output |

The 23 existing optional skips comprise 15 live-service/bridge opt-ins, five disc-dependent
checks, two bridge-latency checks and one design export. No live test was enabled and no
external service or credential was required. Existing replay serialization goldens,
ordinary two-player, ranked and doubles controls remained part of the passing full run.
Remaining required work: none. Blocked: none.

The `-Werror` gate compiles `WebSocket.cpp`, `YouGameRoom.cpp`, `OnlineMatch.cpp`,
`RoomTest.cpp`, and the generated curl `orca-http.c`. The local reproducer is
`build-orca-tests/room-close-0348-werror.py`; it uses this build tree's
`compile_commands.json`, removes dependency-output options, redirects each object
into `room-close-0348-werror/`, and appends `-Werror` without a compiler-cache wrapper.

Full-suite command:

```sh
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 \
  python3 Tools/orca/room-fix-tests.py build-orca-tests/Binaries/Tests/tests \
  --gtest_output=xml:build-orca-tests/room-close-0348-tests.xml
```

All builds and test suites use the shared lock and `nice -n 10`; builds use two jobs
and the unit binary has one test worker. `uptime` was checked before each gate, below
18 for each start. Compiler caching is disabled for builds/scans. Tests run through
`Tools/orca/room-fix-tests.py` with a minimal environment and loopback fixtures; it also
sets `ORCA_NO_LOCAL_INPUT=1` and all six requested SDL settings. No game process was
launched outside the unit tests. No install, external-service request, push or release
operation was performed.
