# YG Dolphin 0.3.48 verification

Task `train-0348`, 2026-10-16. **BLOCKED during build configuration.**
No tests or gameplay cases ran in this attempt. No version or runtime changes were made.

The required base check ran before any changes:

```text
694f5f9644 Merge friendlies-0348 (friends-room fixture controls) into train-0348
BASE-OK
```

The initial worktree was clean, on branch `train-0348`.

## Configuration blocker

Evidence: [train-0348-configure.log](train-0348-configure.log).
CMake failed because the bundled curl source directory
`Externals/curl/curl` does not contain `CMakeLists.txt`; the subsequent
`libcurl_static` target therefore does not exist. The build directory
`build-orca-tests` contains only an incomplete configuration.

The attempt checked one-minute load below 18 (observed 4.21), then ran CMake
under `flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10`.
It used Ninja, Release, arm64, macOS deployment target 11.0, tests and NoGUI ON,
Qt and the macOS bundle OFF, and the macOS packaging dependency/feature flags
with signing OFF. Ccache was enabled with the requested local cache and
read-only remote file cache. No build or fixture process was started.

```sh
CCACHE_DIR=/Volumes/YGWorktrees/ClaudeWorktrees/orca-ccache \
CCACHE_REMOTE_STORAGE='file:/Users/luis/Code/brawl-dolphin/ccache|read-only' \
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 \
cmake -S . -B build-orca-tests -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DENABLE_QT=OFF -DENABLE_NOGUI=ON -DORCA_MACOS_BUNDLE=OFF \
  -DUSE_SYSTEM_LIBS=OFF -DUSE_SYSTEM_ICONV=ON \
  -DENCODE_FRAMEDUMPS=OFF -DENABLE_LLVM=OFF \
  -DENABLE_ANALYTICS=OFF -DENABLE_AUTOUPDATE=OFF \
  -DUSE_DISCORD_PRESENCE=OFF -DUSE_RETRO_ACHIEVEMENTS=OFF \
  -DUSE_MGBA=OFF -DUSE_UPNP=OFF -DENABLE_VULKAN=OFF \
  -DENABLE_CLI_TOOL=OFF -DENABLE_TESTS=ON -DUSE_SANITIZERS=OFF \
  -DENABLE_CCACHE=ON -DMACOS_CODE_SIGNING=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

Task hard rule 8 requires stopping on a block and forbids routing around it.
No dependency switch, download, clone, install, or other workaround was attempted.
The bundled dependency must be supplied before this build can proceed.

## Requested changes and gates

| Item | Result |
| --- | --- |
| A: replay-header golden pins, both seat-label variants | NOT RUN; no golden hex verified |
| A: real chunked stream hex, length and SHA-256 | NOT RUN; no stream artifact generated |
| B: friendlies proof and any required fixes | NOT RUN; build configuration blocked |
| C: version defaults and 0.3.48 player notes | NOT STARTED; preceding items incomplete |
| Full test binary | NOT RUN; 0 tests executed |
| Full `--gtest_filter=Orca*` | NOT RUN; 0 tests executed |
| Doubles fixture self-test | NOT RUN; stopped at configuration blocker |
| Compatibility-key template self-test | NOT RUN; stopped at configuration blocker |
| Touched C++ files with `-Werror` | NOT RUN; no C++ edits |

## Friendlies cases

No fixture runs or gameplay logs were produced. The shared blocker evidence is
[train-0348-configure.log](train-0348-configure.log). The existing friendlies
verification note predates this attempt and is not evidence of a new run.

| Case | Result in this attempt |
| --- | --- |
| 1: three peers, two fight, one sits out | NOT RERUN; prior PASS reported by Luis in the task |
| 2: rotate at next CSS | NOT RUN (BLOCKED) |
| 3a: four peers, two fight, two sit out | NOT RUN (BLOCKED) |
| 3b: all four fight 2v2 | NOT RUN (BLOCKED) |
| 4a: sitting-out peer leaves permanently | NOT RUN (BLOCKED) |
| 4b: sitting-out peer away/back within budget | NOT RUN (BLOCKED) |
| 4c: sitting-out peer blackout/recovery | NOT RUN (BLOCKED) |
| 5: third arrival mid-fight | NOT RUN (BLOCKED) |
| 6: third arrival during another friend's recovery | NOT RUN (BLOCKED) |
| 7: fifth arrival refused alone | NOT RUN (BLOCKED) |
| 8: ordinary room refuses only third arrival | NOT RUN (BLOCKED) |

Only this verification note and its configure log were added. All requested
implementation and validation work remains. No pushes or external service calls
were made, and the authorized disc was not opened.
