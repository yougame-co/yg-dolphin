# nolocal-0349 verification

Worktree: `/Volumes/YGWorktrees/ClaudeWorktrees/orca-0348`, branch `train-0349`.
Required base check before edits:

```text
503c7d9c82 Keep saved replay playback offline
BASE-OK
```

`git merge-base --is-ancestor 503c7d9c82 HEAD` exited 0. The existing untracked
`notes/tests-0349-runs/` is outside this change. All changes stay local; no push,
release, signing, deployment, or request to a real external service is part of this run.

## Local input switch

`InputCommon::LocalInputDisabled()` reads only `ORCA_NO_LOCAL_INPUT`, and only the
exact value `1` enables it. Unset, empty, `0`, and other values retain the previous
branches. Set the variable before launching the process; it is a launch setting.

With `ORCA_NO_LOCAL_INPUT=1`:

- `ControllerInterface::Initialize` creates no input backend. The shared guard
  encloses Win32 (DInput, XInput, WGInput), XInput2, Quartz, SDL, Android, evdev,
  Pipes, DualShockUDPClient/DSU, and SteamDeck. Normal controller-interface setup,
  refresh, window changes, and shutdown still work with the empty list.
- Device refresh does not populate real Wii Remotes. Real Wii initialization
  returns before scanner startup, config reads, and the state-load wait; direct
  `WiimoteScanner::StartThread` is guarded as well.
- `GCAdapter::Init` returns before creating the native libusb context or Android
  adapter class, consulting the emulator, registering config callbacks, or starting
  detection. `GCAdapter::IsInitialized` exposes native initialization for the test.
- Bluetooth passthrough `BluetoothRealDevice::Open` returns `IPC_ENOENT` before
  constructing `LibUSBBluetoothAdapter`, whose constructor would enumerate USB.
- `UICommon::Init` emits one boot log after log initialization:
  `local input off (ORCA_NO_LOCAL_INPUT)`.

Orca's test commands, `YG_INPUT`, controller stream, and room pad input paths are
unchanged. This is shared C++ behavior, including the Windows backend entry point;
this machine validates the macOS build, not a Windows runtime.

Four new `LocalInput` tests cover exact switch values; zero backends and devices
across repeated initialize/refresh/window/shutdown cycles; skipped native adapter
initialization; and Wii initialization including its wait mode without config.
The tests never initialize hardware in an unset-switch control case.

`doubles-rooms.py` already supplied the variable; it now sits beside all six SDL
settings in each peer's environment. `queue-e2e.mjs` explicitly supplies the same
seven settings per instance. Parent SDL overrides cannot replace those values.

## Desktop agent copies (YouGame follow-up)

The agent-copy launch in YouGame `desktop/src/orca.ts` must put
`ORCA_NO_LOCAL_INPUT: "1"` in the environment passed to the spawned YG Dolphin,
after inherited environment/spread values, for both Mac and PC agent/test copies.
Any alternate restart or diagnostic spawn for those copies must use that same
environment. Apply it to the YG Dolphin child, while keeping the app's scripted
controller stream available. `desktop/src/controllers.ts` owns that stream.

No YouGame file is changed here. The source paths and responsibilities above are
identified by this repository's existing `page-e2e.mjs`, `fake_bridge.py`, and
`controller-latency.mjs` references. The separate YouGame checkout was not read
because this run is confined to this worktree. Its current spawn line and
`origin/claude/v2` Player/ticket implementation still need source confirmation.

## Queue ticket build identity

`queue-e2e.mjs` now requires `ORCA_BUILD=<version>:<commit>` for the executable
being run, and adds this to every room and queue ticket request:

```json
{"orca_build":{"version":"0.3.48","commit":"0123456789abcdef0123456789abcdef01234567"}}
```

The example commit is a fixture, not the current build. Supply the actual binary's
YG Dolphin release version and embedded source revision; do not substitute the
checkout HEAD when running a different binary. Whitespace can replace the colon,
and a version alone sends `commit: null`. Missing or malformed build input fails
before starting any Dolphin instance or contacting the ticket endpoint. No build
identity is inferred from Dolphin's upstream `--version` string.

The `{version, commit}` shape matches the in-tree `fake_bridge.py` and
`match-latency.mjs` ticket clients. The dry self-test captures mocked fetch bodies
for room and queue tickets, checks build parsing and serialization, and proves
missing build input causes no fetch. No staging ticket request was made.

## Gates

Every build, compilation check, and test suite runs under
`flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10`, with load checked
before starting. The build uses two jobs; unit tests and explicit `-Werror`
compiles run serially. Evidence is under ignored `build-orca-tests/nolocal-0349-*`.

| Gate | Result | Evidence |
| --- | --- | --- |
| Build `tests` and `dolphin-nogui`, two jobs | Passed, exit 0; no warnings/errors reported | `nolocal-0349-build.log` |
| Doubles-room self-test | Passed, exit 0 | `nolocal-0349-doubles-self-test.log` |
| Queue syntax check | Passed, exit 0 | `nolocal-0349-queue-syntax.log` |
| Queue dry self-test | Passed, exit 0; ticket bodies use mocked fetch | `nolocal-0349-queue-self-test.log` |
| Full binary through room/keyframe fixtures | Passed, exit 0: 2,205 tests; 2,182 passed, 23 skipped, 0 failures; 601.048 s | `nolocal-0349-fixture-tests.log` / `.xml` |
| Full binary directly in a minimal environment | Passed, exit 0: 2,205 tests; 2,112 passed, 93 skipped, 0 failures; 64.421 s | `nolocal-0349-direct-tests.log` / `.xml` |
| Six touched C++ translation units with `-Werror` | Passed, exit 0; all six object compiles clean | `nolocal-0349-werror.log` |

The fixture run's 23 skips are opt-in live/disc/probe checks. The direct run has
70 additional skips because room/keyframe fixtures are not configured there;
those cases ran in the passing wrapper run. All four new `LocalInput` tests pass
in both runs. Both XML reports have zero failures and zero errors.

Commands (each prefixed with the lock and nice command above):

```sh
cmake --build build-orca-tests --target tests dolphin-nogui --parallel 2
python3 Tools/orca/doubles-rooms.py --self-test
node --check Tools/orca/queue-e2e.mjs
node Tools/orca/queue-e2e.mjs --self-test
python3 Tools/orca/room-fix-tests.py build-orca-tests/Binaries/Tests/tests \
  --gtest_output=xml:build-orca-tests/nolocal-0349-fixture-tests.xml
python3 build-orca-tests/nolocal-0349-direct-tests.py
python3 build-orca-tests/nolocal-0349-werror.py
```

The direct test driver creates a temporary `TMPDIR` and passes only a fixed PATH,
`ORCA_NO_LOCAL_INPUT=1`, and the six SDL variables. The fixture wrapper supplies
its own minimal environment with the same switch and loopback-only room/keyframe
URLs. Neither run inherits app credentials, bridge configuration, or live-test
opt-ins. Both runs use the entire test binary without a test filter.

The `-Werror` driver reuses each touched translation unit's real compile command,
changes its output to this build directory, strips dependency-file arguments, and
adds `-Werror`. It compiles `ControllerInterface.cpp`, `GCAdapter.cpp`,
`WiimoteReal.cpp`, `BTReal.cpp`, `UICommon.cpp`, and `LocalInputTest.cpp` serially;
the changed C++ headers are covered by those includes.
