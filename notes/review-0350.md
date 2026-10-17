# Independent review: train-0350

## BLOCKING

1. **B1 — Preserve the Mac session's prohibition on reading global keyboard state.**
   `Source/Core/DolphinNoGUI/PlatformMacos.mm:304`–`309` adds a direct
   `CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, kVK_F8)` poll in precisely the
   embedded stdio-session configuration where `Sandbox::OsKeyboardAllowed()` returns false
   (`Source/Core/Core/Orca/Sandbox.cpp:245`) and startup disables Quartz keyboard input
   (`Source/Core/DolphinNoGUI/MainNoGUI.cpp:612`). Concrete scenario: start an ordinary
   embedded app session, then let the app give the visible game focus. Every main-loop pass
   now queries system-wide keyboard state despite the disabled keyboard device. Neither
   that device's disable switch nor the local-input isolation switch controls this new call.
   The policy test can still pass because it tests the predicate, not this call site. Remove
   the global poll and deliver the shortcut through the focused app's existing `feedback`
   command, without requiring additional macOS permissions.

2. **B2 — Protect the frame dumper's lifetime before requesting feedback.**
   `Source/Core/Core/Orca/UX/YgOrb.cpp:638`–`643` checks and then dereferences the global
   `std::unique_ptr<FrameDumper>` on the window thread without synchronization with its
   initialization/destruction on the video thread. `HostEnabled()` checks the bridge and
   app stop flag, not a live renderer. Concrete no-frame failure scenario: the app sends
   `feedback` during asynchronous boot, after `VideoBackendBase::InitializeShared()` assigns
   the dumper (`Source/Core/VideoCommon/VideoBackendBase.cpp:330`), while initialization of
   another renderer component fails. That failure calls `Shutdown()` at line 344 and resets
   the dumper at line 386. If the reset falls between the check and `SaveScreenshot()`, the
   request dereferences a null/freed object or locks its destroyed mutex. `Core::Init()`
   starts the emulation thread and returns immediately (`Source/Core/Core/Core.cpp:251`),
   so the window's stdin loop can run during this interval. This affects both Windows and
   macOS and defeats the requested no-frame crash guarantee. Dispatch the request through
   an operation that protects the renderer's lifetime and rejects unavailable/starting/
   stopping renderers; another unsynchronized null/state check is insufficient. Add a
   regression for feedback crossing renderer initialization failure/teardown.

## Non-blocking

1. **N1 — Production capture/gating and some report branches lack integration coverage.**
   `Source/UnitTests/Core/Orca/UXTest.cpp:4527` checks the menu model and literal strings;
   it never calls `ActivateHost()` or tests bridge gating and actual emitted host lines.
   The PNG test at line 4577 saves a known 1×1 image synchronously and calls
   `PublishFeedback()` directly. It verifies that helper's rename/result ordering and PNG
   signature, but does not exercise `FrameDumper`'s callback, observe stdout, decode the
   complete PNG, or cover unavailable frames, renderer lifetime, failed close/rename, or
   the size limit. Those tests would continue to pass with B1/B2 present. Similarly,
   `Source/UnitTests/Core/Orca/RoomTest.cpp:4195` exercises ranked begin/start/report and
   a fault finish, not the casual finish, finish retry, or SDK hello branches; the test at
   line 4141 passes invented messages directly to the helper. The source paths appear
   correctly marked, but the tests do not establish integration coverage of every branch.
   Add focused regressions for actual bridge gating, unavailable frames, and callback/stdout
   ordering, and distinguish these helper/model checks from full-path validation in
   `notes/train-0350-verification.md:38` and `:50`.

2. **N2 — Native Windows keyboard handling needs an embedded-focus integration check.**
   `Source/Core/DolphinNoGUI/PlatformWin32.cpp:393`–`413` handles F8, Shift+Tab, and menu
   navigation only when the render HWND receives `WM_KEYDOWN`. In normal embedding,
   `WM_SETFOCUS` immediately returns keyboard focus to the parent (`:485`–`490`), and the
   event loop only consumes the render HWND's messages (`:351`–`357`). Physical keys sent
   to the focused parent therefore do not reach these new handlers. Confirm the app's
   relay and document which keys it supplies; this change supplies stdin commands for
   feedback/menu opening but no menu-navigation relay. Mouse/controller controls remain
   available, so this is recorded as an integration limitation rather than an additional
   release blocker. No Windows native run was performed.

## Other requested checks

- **PNG publication and names:** the normal successful path encodes and explicitly closes
  the PNG (`Common/Image.cpp:119`), then invokes the completion callback
  (`VideoCommon/FrameDumper.cpp:229`), then renames the temporary file before returning and
  emitting the feedback line (`Orca/UX/YgOrb.cpp:609`–`651`). The generated ASCII basename
  fits `[A-Za-z0-9._-]{1,64}.png`; publication enforces the same character/length rules.
  Both paths are in `File::GetUserPath(D_USER_IDX)`, with a `.tmp` suffix until success.
  Failed saves/empty or oversized files/failed renames emit no feedback line. The pending
  flag prevents overlapping feedback captures and the completion timestamp enforces at
  least five seconds between successful publications. No synchronous frame dereference
  occurs merely because no XFB has arrived; lifetime protection remains B2.
- **Windows file I/O:** the existing `IOFile` Windows path conversion and filesystem rename
  are reused. Explicit close precedes same-directory rename, avoiding an open-writer
  rename failure on Windows. The production basename avoids Windows reserved device names
  and path separators. No additional file-write/rename portability defect was found by
  source inspection; this is not a Windows execution result.
- **Bridge and simulation:** all new production action entry points pass through
  `HostEnabled()`, which requires a session and a ready stdio bridge. Host actions use the
  five specified lines, with Controls using `orca orb`. Controller navigation does not
  rewrite the returned pad; the existing Up+Start mask remains the only filter. No new
  guest-memory writer or rollback/simulation rule was introduced.
- **Harness markers:** both hello serializers are conditional. Begin, ranked start/report,
  casual finish, final finish and its saved retry text, and app results all pass through
  the marker helper. With both opt-ins absent, that helper returns the original bytes and
  neither hello object receives another field. No normal-play serialization change was
  found. The test-driver opt-in is checked before the first pad snapshot.
- **Builtin regions:** exactly five new disjoint ranges add 52 bytes; existing ranges are
  unchanged. They cover only the ten My Music cleanup words, the restored pause word,
  and the scene manager's exit-code/step words, preserving the holes and adjacent words.
  `0x805B8E24` is correctly attributed to `FriendsMove::Apply()` and `FreshMove::Apply()`
  through `0x805A0060 -> 0x805B8BA0 + 0x284`, not to the results writer. The new fake-memory
  tracking records every byte of the exercised writes, and boundary assertions check the
  exclusions. The declarations feed write auditing, not guest execution/state changes.

## Scope and validation

- Reviewed `ce0a4d906a..adeced6bc3ffc5aa7b2e612b136356107f7c77df` on `train-0350`,
  including the train verification note and writer inventory. Initial status was clean.
- Base check output: `adeced6bc3 Record train 0350 verification`; `BASE-OK`.
- Evaluated the explicit contract invariants in the brief. The full contract resides at
  the supplied path outside this worktree and was not opened under the worktree-only
  restriction; no claim is made about additional clauses in that external document.
- No build, native window run, external-service request, or production action. Existing
  `build-orca-tests/Binaries/Tests/tests` ran through `Tools/orca/room-fix-tests.py` under
  the shared `flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10` lock after `uptime`
  showed a 1-minute load of 5.34. **PASS: 21 tests across 9 suites, no failures or skips,
  exit 0.** The selection covered all YgOrb tests, the three new harness room tests, app
  set results, the four changed region tests, and the existing Mac keyboard policy test.
  Fixtures used loopback endpoints and a minimal environment with local input disabled.
- `git diff --check ce0a4d906a..HEAD` passed. Only this review note is to be committed.
