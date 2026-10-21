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
