# train-0353 verification

Base: `7b2e45efea YG Dolphin 0.3.52`; ancestor check: `BASE-OK`. No version bump, no push.

## Item 1 — embed trace

Added opt-in `ORCA_EMBED_TRACE=1` logging at applied rect/view commands. Backend bounds queries
read only our own NSWindow frame / HWND / X11 window, and run only when the switch is exactly 1.
Format: `embed-trace t=<steady-clock ms> req=<x,y,w,h> real=<x,y,w,h> scale=<s>`.
`view off` reports the active rect as req; too-small rects log their raw request and the
unchanged real bounds after hiding.
macOS real uses Cocoa screen points; Windows real uses physical screen pixels; X11 uses screen
pixels with scale 1. No new window enumeration APIs. Runtime GUI checks intentionally omitted.
Build and test gate results are recorded below after all four items.

## Item 2 — self-embedding result

Added a first-decision latch shared by self-embedding backends, held until ready. Initial native
rect placement counts; hidden/minimized starts wait for actual visibility. Initial embed failure
falls back to a standalone window while retaining the protocol. A parent lost after placement
still stops as before. Adopted sessions disable the latch.
Exact stdout: `orca embed ok` or `orca embed failed <reason>`, where reason is one of
`parent-gone`, `setparent`, `no-parent`, `track-failed`. Never before ready, never twice.
Documented in Status.h and ORCA.md Embedding. Unit tests cover ready gating, first decision,
each failure token and disabled/adopted silence. Native UI checks omitted per task instructions.

## Item 3 — boot allocation failure and memory log

Catch bad_alloc at the EmuThread boot/run boundary (inside its terminal-state guard), plus the
synchronous NoGUI BootCore call. Existing scope guards unwind before reporting. The shared
callable seam allocates nothing and rethrows after the presenter has observed the first frame
(also without an Orca profile); other exception types retain their behavior. Partial HW init is
not passed to full HW shutdown. Non-MSVC builds explicitly enable exceptions for Core.cpp,
BootMemory.cpp, MainNoGUI.cpp, the known cache allocator PPCCache.cpp and the test using the
existing per-source pattern; no global exception-mode change. Trace uses the normal ROLLBACK
logger enabled by the embedded NoGUI boot; memory diagnostics use its normal listeners even
with category filtering disabled.
Exact Status line: `orca error low_memory Your PC ran out of memory while starting the game. Close other apps first.`
The existing Status finish path retains exit 1 and `orca state ended`, suppressing later errors.
Memory is logged once when BootCore starts and again on errors/reports, log only:
`memory: total <MB> MB, available <MB> MB[, commit <used>/<limit> MB]`.
Windows uses GlobalMemoryStatusEx, macOS hw.memsize + free/inactive host pages, Linux MemTotal /
MemAvailable. MB are integer MiB. Tests inject a throwing boot callable, verify cleanup, exact
error, first-error suppression, exit/ended, propagation after first frame and other exceptions.

## Item 4 — preserve INI index bytes

Applied the lead addendum: `*.ini -text` with the exact requested comment. No renormalization,
no INI edits. `git ls-files --eol '*.ini' | awk '{print $3}' | sort | uniq -c` reports
`1885 attr/-text`. `git diff --exit-code 7b2e45efea -- '*.ini'` passes with no output.
RGWP41.ini's index blob remains `9f3c7f87c846852ed883e78dfe9260bf5a82346d`, identical to base;
its upstream CRLF bytes are preserved. Final committed worktree cleanliness is checked below.

## Verification gates

Build configuration: build-orca-tests, Ninja, Release, ENABLE_NOGUI=ON, ENABLE_QT=OFF,
ENABLE_TESTS=ON, USE_SANITIZERS=OFF. Builds use six jobs; the single test binary runs sequentially.
Every build/test/strict compilation runs under
`flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10`, with
`CCACHE_DIR=/Volumes/YGWorktrees/ccache` for builds. Load was checked before starting jobs
and stayed below 18; no load wait was needed. Only unit tests ran; no game instances launched.
The no-local-input switch and all six specified SDL variables were set for tests.

Focused tests: `--gtest_filter=OrcaBootMemory*:OrcaEmbed*`: 5 passed, 0 failures.
The initial full run exposed test-child exit cleanup of inherited threads; the fixture now uses
an immediate child exit and a DeathTest suite so it runs before threaded suites.
Final full binary: 2,234 tests from 179 suites ran, 2,139 passed, 95 skipped, 0 failures
(exit 0, 69.387 s). Skips are opt-in live/disc/bridge/keyframe/kit/room-runtime fixtures; no
credentials or external-service setup was supplied.

Final build: `cmake --build build-orca-tests --target dolphin-nogui tests --parallel 6` passed.
It retains the pre-existing unused BUTTON_START warning in untouched Games/Brawl/Queue.cpp.
Strict compilation: each of the 10 touched host C++/Objective-C++ sources was rechecked with its
compile_commands.json flags plus `-Werror -fsyntax-only`: all passed. Windows/X11 backends were
reviewed but cannot be compiled or exercised on this macOS host. No dependencies were installed.
Build/test/strict logs are retained under build-orca-tests/train-0353-*.log.
`--gtest_filter=Orca*`: 1155 tests from 122 suites ran, 1081 passed,
74 skipped, 0 failures (exit 0, 64.559 s).

Final checks: `git diff --check` passes; no tracked INI differs from base; all 1,885 INIs
show attr/-text. After committing this rule and these notes, `git status --porcelain` is empty.
The unit suite's generated imgui.ini was moved under build-orca-tests/; no untracked files
remain outside that ignored build directory. No version bump or push. Nothing blocked.

## hostui-0353 — negotiated native host menu

Base check before edits:

```text
11e16e5e7b Preserve INI checkout bytes across platforms
BASE-OK
```

Read the requested compatibility note (section 7) and compared the UX, NoGUI and bridge changes
from 0.3.49 (`ce0a4d906a`) through the task base (`11e16e5e7b`). The new engine capability is
`hostui`, offered after `record`, last so truncating app readers drop the newest word first.
The ordinary ruleset offer now has 21 words, within the desktop's 24-word limit; the existing
optional replay word remains before record/hostui. No existing word is removed or reordered.

Before this change an active stdio session with a writer enabled the native host menu regardless
of the app's support. Now `YgOrb::HostEnabled()` also requires `Status::Cap(HOSTUI_CAP)`, using
the intersection of the current engine offer and the app's complete caps answer. This gates the
fallback orb, delta/menu drawing, menu actions/output, feedback capture/cooldown, controller and
keyboard navigation, orb clicks and Mac hit-box interception through their existing entry points.
The feedback stdin handler silently ignores the command without the cap; host-menu uses the
same gated toggle. A later caps answer takes effect immediately and withdrawing hostui closes
an open menu, including an off/on pair between UI polls. Screenshot completion already checks
HostEnabled before emitting output, so withdrawal also suppresses that output.

Without agreement, the app drives the original orb placement, Windows orb clicks take the
existing `orca orb` fallback, Mac clicks pass through, and Up+Start uses the original overlay
press/release with talk or the `orca orb` toggle without talk. Existing overlay-key handling and
shortcut pad masking are unchanged. With agreement, the 0.3.50 native controls remain available.
ORCA.md Embedding, stdin/stdout and caps docs describe the agreement; desktop 0.13.0 / V2 web
will opt in, while the listed production desktops and V1 retain the legacy path.

Three new UX tests cover absent agreement with a live writer (no host/feedback output, screenshot
cooldown consumption, native drawing or hit boxes; legacy orb/shortcut/key paths), enabled controls, and
mid-run caps changes including both kinds of in-flight shortcut press. Existing screenshot,
five-second cooldown, host rate, renderer and controller tests are retained; their renderer test
now explicitly agrees hostui. Caps goldens for legacy/mod/pack profiles include the appended word,
and the ordinary-offer test asserts exactly 21 words and the newest-last suffix.

Exact ordinary offers (pinned by OrcaMod.LegacyCapsLinesArePinned):

Brawl rev 2:
```text
orca caps join leave stats pause delay perf direct host chat yougame gfx music results locks queue2 pick-casual pick-ranked talk rumble record hostui
```

Project+:
```text
orca caps join leave stats pause delay perf direct host chat yougame gfx music results locks queue2 pick-casual pick-ranked talk rumble record hostui
```

### hostui-0353 gates

All I/O-heavy checks used `flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10`.
Build/compiler jobs used at most two workers and `CCACHE_DIR=/Volumes/YGWorktrees/ccache`;
unit binaries ran sequentially in a minimal environment with ORCA_NO_LOCAL_INPUT=1 and all six
requested SDL isolation variables. Loads were checked before starting each job and during the
shared-lock wait; they stayed below 18. No dependency installs, game instances or native UI smoke
runs were needed. Windows input fallback was reviewed in source; native Windows UI was not
exercised on this macOS host.

- Build: `cmake --build build-orca-tests --target dolphin-nogui tests --parallel 2`, PASS, exit 0.
  The existing unused BUTTON_START warning remains in untouched Games/Brawl/Queue.cpp.
  Log: build-orca-tests/hostui-0353-build.log.
- Focused regressions: 28 tests from 6 suites, 28 passed, 0 failed, exit 0.
  Log: build-orca-tests/hostui-0353-focused.log.
- Full unfiltered binary: 2,237 tests from 180 suites; 2,142 passed, 95 skipped, 0 failed,
  exit 0 (69.767 s). Log: build-orca-tests/hostui-0353-full-tests.log.
- `--gtest_filter=Orca*`: 1,158 tests from 123 suites; 1,084 passed, 74 skipped, 0 failed,
  exit 0 (65.008 s). Log: build-orca-tests/hostui-0353-orca-tests.log.
  Skips are the existing opt-in fixtures; no live-service or disc setup was supplied.
- Strict compilation: the existing notes/train-0350-werror.py runner, explicitly passed all nine
  touched C++ sources plus OrbCombo.cpp, YgOrbDraw.cpp and PlatformMacos.mm, compiled all 12 with
  their actual compile_commands.json flags plus `-Werror`; 12 passed, exit 0.
  Log: build-orca-tests/hostui-0353-werror.log.
- `git diff --check`: PASS. Test-generated files remain inside the ignored build directory.

No version bump, push, PR, release or deployment. Nothing blocked or left for this task.
