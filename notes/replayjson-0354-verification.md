# replayjson-0354 verification

Private local engine change on `fix-replay-header-0354`. No push, PR, release,
deployment, signing, dependency installation, or external-service request.

## Base

Required check, before edits:

```
343a655173 Keep boot-memory test portable to MSVC
BASE-OK
```

`git merge-base --is-ancestor 343a655173 HEAD` succeeded.

## Shared reader and call-site audit

`Source/Core/Core/Orca/SafeJson.{h,cpp}`, namespace `Orca::Json`, carries the room
protocol's `SanitizeJson` and `ParseJson`. The existing cleanup behavior and limits
are preserved: depth 16, at most 17 consecutive numeric digits, exponents replaced
with null, quoted text preserved, and unclosed strings/brackets refused. The private
string/bracket/number helpers move with them. The default Room number policy retains
the original room cleanup rules. Controller snapshots and bridge lines retain their
existing bounded-input preflight and explicitly select Finite numbers: decimal
precision and finite exponents are preserved, while numbers are checked for finite
conversion before picojson reads them. The nesting cleanup is shared by both modes. Both files join the core CMake
sources.
`ParseWholeJson` applies the same cleanup while retaining the mod reader's single
value and trailing-whitespace requirement.

Every formerly direct external-data parse site found by searching the complete
`Source/Core/Core/Orca` tree, plus `Rollback/ReplayStream.cpp` (the rest of
Rollback was checked too, with no further direct parse calls):

| File (original line) | Input | Shared reader |
| --- | --- | --- |
| Session/Keyframe.cpp:726 | Replay stream header from a file or watch stream | ParseJson |
| Session/Keyframe.cpp:1423 | HTTP keyframe-store refusal body | ParseJson |
| Rollback/ReplayStream.cpp:701 | Watch-ticket HTTP response | ParseJson |
| Rollback/ReplayStream.cpp:835 | Watch WebSocket text | ParseJson |
| Bridge.cpp:206 | Inbound app bridge line | ParseJson, Finite numbers; existing preflight retained |
| UX/Controllers.cpp:305 | Inbound controller snapshot event | ParseJson, Finite numbers; existing preflight retained |
| Mod.cpp:125 | File-backed mod.json and tree.json through ParseWhole | ParseWholeJson; existing size/depth checks retained |

The nine existing room callers also use the shared copy (original YouGameRoom.cpp
lines 857, 1020, 1597, 1801, 2779, 2791, 2837, 3363, 4138). These cover packet decode,
the exported room parser, HTTP replies, bridge requests/replies, room lines, saved
sent details, and socket messages.

No compile-time/static JSON parse sites were found in the production tree being
audited. Unit-test fixture parsing stays unchanged. The separate engine strict JSON
reader stays unchanged. The only direct picojson parse calls remaining in the
production Orca tree are the two shared-reader implementations, both after cleanup.

## Regression tests

New normal in-process tests (no death tests):

- OrcaReplayStream.HeaderWithHugeNumberIsRefused: 1e999 and a 400-digit integer.
- OrcaReplayStream.HeaderNestedPastLimitIsRefused: exactly 16 KiB of opening
  brackets (within the container's header-size limit), plus a balanced nested field.
- OrcaReplayStream.WatchMessageWithHugeNumberIsRefused: both numeric forms in
  WebSocket JSON and the carried replay header; ordinary playback refusal.
- OrcaReplayStream.WatchMessageNestedPastLimitIsRefused: 16 KiB of opening brackets
  in the carried header, balanced nesting in the WebSocket JSON, and a second
  balanced nested-header case; ordinary playback refusal.
- OrcaReplayStream.ValidHeaderParsesByteIdentically: feed one byte at a time and
  repack to exactly the original bytes, alongside the existing golden fixture test.
- OrcaSafeJson.PreservesFiniteControllerNumbers: preserve controller decimal
  precision and finite exponents, retain the default room cleanup, and clean
  both large-number forms before parsing.
- OrcaMod.RefusesHugeJsonNumbersAndTrailingValues: both numeric forms through
  manifest/tree readers, plus the existing trailing-value refusal behavior.

Header refusals leave no accepted header or frames and remain failed on a later
valid feed. Watch tests use an in-process loopback WebSocket fixture and end before
restoring an origin or playing frames.

Before-change run: the baseline executable was built before the shared reader was
wired into any production caller. Exactly one invocation of
`--gtest_filter=OrcaReplayStream.HeaderWithHugeNumberIsRefused` started one normal
test and returned **-6 (SIGABRT)** before completing it. Core files were disabled.
The runner records the raw child result in
`build-replayjson/replayjson-0354-before.log`. Its own success status confirms the
expected baseline result, not a passing regression. The executable was preserved
unchanged while that command waited for the shared lock.

Initial after-change full run: 2,244 tests, one failure in the existing
OrcaUXControllers.ParsesTheHelpersSnapshot test. All six new regressions passed.
The fixture uses valid axis decimals longer than 17 digits; the explicit Finite
mode above preserves those values in controller and bridge data. A new regression
covers both numeric modes. Strict compilation of the initial change passed all nine
touched translation units. Final after-change results: pending.

## Build and gates

Configured `build-replayjson` with Ninja, Release, ENABLE_NOGUI=ON, ENABLE_QT=OFF,
ENABLE_TESTS=ON, USE_SANITIZERS=OFF, CMAKE_EXPORT_COMPILE_COMMANDS=ON.
USE_BUNDLED_MOLTENVK=OFF keeps this test build local; MACOS_CODE_SIGNING=OFF.

Builds use CCACHE_DIR=/Volumes/YGWorktrees/ccache and
CCACHE_REMOTE_STORAGE=file:/Users/luis/Code/brawl-dolphin/ccache|read-only.
Every configuration, build, strict compilation, and test command uses
`flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10`.
Build parallelism is six; tests run sequentially. Load was checked before jobs;
load waits resume below 18. Read-only load samples and logs are in build-replayjson.
Tests use a minimal environment with local input and all six SDL device variables
disabled; no external-service fixture setup is supplied.

Final gate results: pending.
