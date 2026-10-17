# juice-0350 verification

A networking fix for Windows: drop an oversized datagram instead of failing the link.

## Scope and base

- Branch: `train-0350`.
- Starting output: `f2b6f3c3b0 Adjust host controls and screenshot requests`; `BASE-OK`
  from `git merge-base --is-ancestor f2b6f3c3b0 HEAD && echo BASE-OK`.
- Upstream libjuice: `b89c792e3612faf2f12cf35bcc56857313a06be3` (1.7.4).
- The submodule and `.gitmodules` stay pristine. No version or `ORCA.md` changes.
- Reference read in the preceding run: YouGame commit `74846cbc5`, `desktop/native/link/patch-libjuice.cmake`
  and `desktop/native/link/tests/`.

## Configure-time copies

`Externals/libjuice/CMakeLists.txt` reads the three upstream source files, applies exact
string replacements, and compiles copies in its binary directory. Each anchor must occur
exactly once; missing or duplicate anchors stop configuration with `FATAL_ERROR`. CRLF
line endings are normalized before matching. Source changes trigger reconfiguration;
unchanged generated content keeps its timestamp.

Eight replacements:

| Source | Anchors |
| --- | --- |
| `conn_poll.c` | Receive-loop branch; fairness-result branch; receive-helper log branch |
| `conn_thread.c` | Receive-helper log/return branch; receive-loop condition |
| `conn_mux.c` | Receive-helper log/return branch; receive-loop branch; final result branch |

All three modes drop an oversized datagram instead of failing the link. Poll and mux
count each receive toward their existing fairness limit. The corresponding message uses
verbose logging. Other receive errors retain their existing handling.

## Regression coverage

`OrcaJuicePollTest` includes the same configured `conn_poll.c` as the library, with private
symbol names and stub UDP receive, agent callbacks, and log writes. It adds no production
hook. The injected receive result passes through the actual receive helper and poll loop.
Five tests cover continued delivery, the fairness limit, delivery on the next poll, other
receive errors, and normal receive/would-block handling; they also check logging levels.

## Gates

Machine commands use the shared build lock and `nice -n 10`, with
`CCACHE_DIR=/Volumes/YGWorktrees/ccache`; builds use at most six jobs and tests run
as one process. Load was checked before commands; the load guard checks again after
acquiring the lock and defers work at 18 or above. No downloads or submodule initialization.

Both configurations use exactly:

```text
-G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_NOGUI=ON -DENABLE_QT=OFF
-DENABLE_TESTS=ON -DUSE_SANITIZERS=OFF
```

- `build-juice-check` was created new for this run. The initial configure rejected the
  carried-over fairness anchor because it matched UDP and TCP. The anchor now includes
  the UDP-specific would-block log branch. Configuration then passed with all eight
  replacements, and the three generated diffs were inspected.
- Existing `build-orca-tests`: configure and `tests` build **PASS**.
- `-Wall -Werror -fsyntax-only`: **PASS, five translation units**, using the configured
  compile commands with `-w` removed: the three generated C sources, the test harness,
  and the C++ test source.
- **Red:** replace only the generated `build-orca-tests/Externals/libjuice/conn_poll.c`
  with the pristine upstream file, verify byte equality, rebuild `tests`, then run
  `tests --gtest_filter=OrcaJuicePoll.*`. **Exit 1: five tests, three failed, two passed.**
  Failures: continued delivery, fairness limit, and next-poll delivery. The other-error
  and ordinary-receive tests passed. The submodule was never modified.
- **Green:** restore the generated patched file, rebuild `tests`, and run the same filter.
  **Exit 0: five tests, five passed, zero failed.**
- Final `cmake --fresh -S . -B build-juice-check` with the options above: **PASS**
  from a cleared configure cache. Fresh `juice` target build: **PASS**.
- Standalone configure probes using the actual patch code: **PASS**. CRLF input copies
  produce byte-identical generated sources. A missing anchor and a duplicate anchor
  each stop configuration with the expected `FATAL_ERROR`.
- Full binary, `build-orca-tests/Binaries/Tests/tests`: **PASS, exit 0**. **2,228 tests
  from 176 suites: 2,133 passed, 95 skipped, zero failed**. The final run after regeneration took 70.009 seconds; the first full
  run had the same counts in 80.485 seconds. Skips are
  the suite's conditional fixture/platform/live-service skips; no external services
  were enabled for this run.
- `git diff --check`: **PASS**. The upstream submodule and `.gitmodules` are unchanged.
- No blockers remain. The previous run's sanitizers-cmake configure issue was resolved
  with the specified `USE_SANITIZERS=OFF` option.

## Artifacts and limits

Build directories remain in place. Logs and the standalone verification scripts are kept
locally in `build-juice-check/juice-0350-verification/` (untracked build artifacts).
The host is macOS arm64 using AppleClang and Ninja. Windows/MSVC and Linux were not run
on this machine; their existing socket constants and build definitions are retained.
No version or `ORCA.md` changes, external-service calls, pushes, or releases.

## juice-e2e-0350: real UDP regression

- Starting output: `b63200da69 A networking fix for Windows`; `BASE-OK` from
  `git merge-base --is-ancestor b63200da69 HEAD && echo BASE-OK`.
- Added `OrcaDirectLink.AnOversizedDatagramLeavesTheLinkUp` next to
  `TwoOrcasLinkAndRebuild`. Two Mesh peers first exchange payloads, then a plain
  platform UDP socket sends three 5,000-byte datagrams and one 9,000-byte datagram
  of zeros to peer 1 over IPv4 loopback. The destination port comes from the local
  selected pair via `juice_get_selected_addresses`; the agent's wildcard-bound
  socket accepts loopback on that same port. `DirectStats::test_local_address` is
  populated only with the existing `DirectOptions::loopback` test knob enabled.
- For two seconds the test checks both generations, peer 1's Direct state, and
  `AllUp()`, then requires fresh payload delivery both ways and unchanged generations.
  Mesh only collects `DirectLink::Receive()`: it has no relay payload delivery.
- An oversized datagram is dropped, the link stays up. On macOS/Linux the receive
  is silently truncated, so the real-socket test is expected to pass with upstream
  libjuice too; the test comment records this platform distinction. Windows/MSVC
  must provide the red/green result on the PC; it was not available on this Mac.
- The optional second two-agent libjuice fixture was omitted to keep this change
  small; the existing five poll tests remain in the full binary.
- `ORCA_JUICE_UNPATCHED` is a test-only CMake option, default **OFF**, rejected
  unless `ENABLE_TESTS=ON`. ON uses unchanged upstream content for all three
  configure-time copies, including the copy used by the poll harness. Configuring
  ON and comparing all three generated files with normalized upstream sources
  **PASS**; restored OFF, reconfigured and rebuilt before the final full run.
- Release / Ninja / NoGUI / tests ON / `USE_SANITIZERS=OFF`, target `tests`:
  **PASS**, including the final build after restoring OFF.
- `--gtest_repeat=10 --gtest_filter=OrcaDirectLink.AnOversizedDatagram*`:
  **10 runs, 10 passed, zero failed**.
- Full DirectLink coverage (`--gtest_filter=OrcaDirectLink.*`):
  **16 tests, 16 passed, zero failed** (32.794 seconds).
- Full `build-orca-tests/Binaries/Tests/tests`: **PASS, exit 0**, 2,229 tests
  from 176 suites: **2,134 passed, 95 skipped, zero failed** (69.475 seconds).
  Skips are the suite's conditional fixture/platform/live-service skips; no external
  services were enabled. `git diff --check`: **PASS**. The upstream submodule and
  `.gitmodules` remain unchanged.
- Configured compiler commands plus `-Wall -Werror -fsyntax-only`:
  **PASS** for `DirectLink.cpp` and `DirectLinkTest.cpp` (also covers the touched header).
- Builds, compiler checks and test runs used the shared `flock` and `nice -n 10`,
  with `CCACHE_DIR=/Volumes/YGWorktrees/ccache`, at most six build jobs and one
  test process. Load was below 18 before each command; no load wait was needed.

### Windows/MSVC commands (from an MSVC developer shell, with Ninja available)

The executable belongs to target `tests`; `OrcaDirectLinkTest` is an object library,
not a separate executable. With the existing dependencies present, build and run:

```powershell
cmake -S . -B build-orca-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_NOGUI=ON -DENABLE_QT=OFF -DENABLE_TESTS=ON -DUSE_SANITIZERS=OFF -DORCA_JUICE_UNPATCHED=OFF
cmake --build build-orca-tests --target tests --parallel 6
.\build-orca-tests\Binaries\Tests\tests.exe --gtest_repeat=10 --gtest_filter=OrcaDirectLink.AnOversizedDatagram*
```

For the Windows red run, retain the test and select upstream sources:

```powershell
cmake -S . -B build-orca-tests -DORCA_JUICE_UNPATCHED=ON
cmake --build build-orca-tests --target tests --parallel 6
.\build-orca-tests\Binaries\Tests\tests.exe --gtest_filter=OrcaDirectLink.AnOversizedDatagram*
```

Expect a nonzero exit and a failed test on Windows. Restore the default with
`cmake -S . -B build-orca-tests -DORCA_JUICE_UNPATCHED=OFF`, rebuild target `tests`,
and rerun the ten-repeat command above: expect zero failures. Run only the new
filter for the unpatched comparison; the existing poll harness tests expect the fix.
