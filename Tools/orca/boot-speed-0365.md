# Boot speed 0365 validation

Base: `8a0b69be6f`, `BASE-OK`. Private local commits on `train-0362`.

## Startup pacing

`BootThrottle::Begin` temporarily removes host throttling before `BootCore`. The first game
boundary restores it, unless a ring port or unthrottled harness owns pacing. Existing temporary
throttle owners are preserved. No emulated timing settings change.

Built `tests` and `dolphin-nogui` in `build-0362`, with two workers. Four new
`OrcaBootThrottle.*` tests pass. All three changed C++ translation units pass their build's
compile command with `-Werror -fsyntax-only`. Existing external-library deployment-target
linker warnings remain.

Mac M4 Pro, headless `-p headless -v Null`, no audio output, local input disabled and all six SDL
controller overrides from the brief. All builds and boots used heavy-slot, build.lock and nice 10.
The disc was read directly from the authorized WBFS. P+ used the official read-only launcher and
SD card in `/Volumes/YGWorktrees/OSCOO/pplus-3.2/run` (no copy).

Spawn to first game frame boundary, milliseconds, three warm runs:

| Game | Baseline | Startup pacing change |
|---|---|---|
| RSBE01 | 399.413, 385.502, 386.690 | 348.596, 354.223, 346.505 |
| PPLUS32 | 2659.595, 2675.645, 2654.428 | 560.728, 566.741, 568.691 |

The runner timestamps the first `Scene frame 0` / `Watch frame 0` diagnostic on the console;
this measures the first emulated boundary, not a GPU presentation. Runs use `YG_EXIT_AFTER=600`,
`YG_HASHLOG`, `YG_SCENES=1`, `YG_WATCH=80000000`. The harness takes over pacing from frame 0
in both binaries. Baseline boot is throttled, candidate boot is unthrottled.

Each candidate warm run's hashlog frames **0 through 600 inclusive** was compared byte for byte
with baseline: **601 lines per game per run, 3,606 lines total, zero differences**. The harness
requests asynchronous shutdown at frame 600 and may log extra frames while the host services
that request; those frames beyond the agreed N=600 are excluded. Frame 0 / frame 600 hashes:

| Game | Frame 0 | Frame 600 |
|---|---|---|
| RSBE01 | 553da1917d394108 | 8b7577e60071c841 |
| PPLUS32 | 3b7d2fd81e2ef856 | c782713deaeb18c1 |

This is the harness's per-frame RAM hash proof, not a bytewise comparison of device savestates.
Logs, complete hashlogs, the baseline binary and measurements are retained in
`build-0362/boot-speed-0365/` (baseline binary in `build-0362/Binaries/`).

Compatibility code fingerprint: `f86645e20656efe5 True`.


## Launcher verification

The legacy launcher profile checks a cache record under `<user>/Cache/OrcaBoot/` containing
canonical path, size, nanosecond mtime and ctime (NTFS LastWriteTime and ChangeTime), volume,
inode / 128-bit Windows file ID, verified XXH3-128 and engine revision. Every field and the
expected hash must match. Missing metadata or malformed records miss conservatively.

Every start launches a worker which fully hashes both files. Cache misses wait, preserving
verification before guest execution. Hits can boot offline while the worker audits; online
Start and StartRoom both reach StartLocked's audit gate before any room constructor, queue
or ticket. A mismatch deletes the failed record, reports the existing `profile` refusal and
stops the session on the host thread. Failure completed during core initialization is delivered
when the session watcher is registered. Shutdown also awaits the audit so an early offline exit
cannot leave a refusal out of the final status. Metadata changes during hashing refuse the boot.

CompatibilityKeyParts is unchanged: it still uses the profile's EXPECTED launcher and SD
hashes. No computed or cached value is copied into those fields. Mod mode retains its existing
bundle/SHA-256 and generated-card verification; it is a separate path, not this helper.

The audit checks below live in `Source/UnitTests/Core/Orca/BootAuditTest.cpp`. The 16 draft
checks are retained, with four additional cases. Tests that check the process-wide refusal
status execute in fresh processes, including on macOS after configuration threads have started.

| Required case | Test names (suite prefix omitted) |
|---|---|
| 1. Warm return while hashing is pending | `ColdVerificationPopulatesAWarmRecordAndStillAudits` |
| 2. Full audit on a hit | `AllLauncherFilesAreAuditedOnEveryStart` |
| 3. Online start waits | `StartCannotRequestATicketUntilTheAuditPasses`, `StartRoomCannotOpenARoomUntilTheAuditPasses` |
| 4. Existing refusal, dropped record, no room | `FailedWarmAuditDropsTheRecord`, `FailedAuditRefusesStartAndStartRoomWithoutARoom`, `MismatchCannotRequestATicketOrOpenARoom`, `FailureCompletedDuringInitializationStillEndsTheSession` |
| 5. Changed bytes, matching cache identity | `ChangedBytesWithMatchingCacheIdentityRefuseBeforeOnline` |
| 6. Size / mtime / file ID changes miss | `SizeChangeIsAMiss`, `ModificationTimeChangeIsAMiss`, `ReplacedFileIsAMissEvenWithTheSameSizeAndMtime` |
| 7. Compatibility key unchanged | `CompatibilityKeyUsesExpectedHashesWithOrWithoutTheCache` |
| 8. Damaged / unreadable cache misses | `CorruptCacheRecordsAreMisses`, `UnreadableCacheRecordIsAMiss`, `PermissionDeniedCacheFileIsAMiss` |

Change-2 `tests` and `dolphin-nogui` builds pass. The focused
`*BootAudit*:*Profile*:*Online*:*Compatibility*` run passes **142/142**, including **20/20**
audit tests and all eight required cases above. The five touched translation units pass
`-Werror`. Compatibility code fingerprint: `f86645e20656efe5 True`.

The first full-suite run had one existing TURN classification failure
(`OrcaDirectLink.FourFriendsExerciseAllSixTurnPairs`); ten isolated repetitions then passed
without changing that code or test. The full-suite rerun through
`python3 Tools/orca/room-fix-tests.py build-0362/Binaries/Tests/tests --gtest_brief=1` passes:
**2,370 tests, 2,347 passed, 23 skipped, zero failures** (192 suites, 628.757 s).
The skips are optional fixture/asset checks; none of the required audit cases skipped.

Three warm PPLUS32 runs, using the same headless Null / no-audio / frame-0 diagnostic as the
change-1 measurements above:

| Run | Change 1 alone (ms) | With launcher cache + audit (ms) |
|---|---:|---:|
| 1 | 560.728 | 346.807 |
| 2 | 566.741 | 353.715 |
| 3 | 568.691 | 347.481 |
| Median | 566.741 | 347.481 |

The warm median is **219.260 ms (38.7%) lower** than change 1 alone. The change-1 measurements
are the recorded baseline above. Both persisted cache identities matched the canonical path,
size, nanosecond mtime / ctime, volume and file ID immediately before each timed cache run.
One priming run is excluded from the warm statistics. Every boot used the authorized read-only
disc and official read-only P+ files, local input disabled, and all six SDL overrides.

Each warm run's frames **0 through 600** matched the change-1 baseline byte for byte:
**1,803 lines compared, zero differences**. The priming run also matched all 601 lines. Logs,
hashlogs, timing JSON and gate outputs are in `build-0362/boot-speed-0365/change2-*`.
