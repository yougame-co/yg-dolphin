# review-0349

Independent source review of `b63badef57..5470249f28` on `train-0349`, including
all five commits and their verification/research notes. No implementation files
were changed. Line numbers below refer to the reviewed HEAD.

## BLOCKING

### B1 — File playback still has a Balance Board UDP path

**Fixed:** Both Balance Board dump sites now check `Common::OfflineReplay()` before
constructing the UDP socket. The fake-board regression covers read and write sends to
a loopback receiver with local input enabled, including a missing replay file and
unset/empty/live-source controls.

**Location:** `Source/Core/Core/HW/WiimoteReal/WiimoteReal.cpp:322` (also `:303`);
policy introduced at `Source/Core/Common/Network.cpp:32`.

**Failure scenario:** Start with a file `ORCA_REPLAY_SOURCE`, a configured real
Balance Board and positive `Core/BBDumpPort`, with `ORCA_NO_LOCAL_INPUT` unset.
`WiimoteReal::Initialize` still starts its scanner. Connecting the board calls
`TryToConnectWiimoteToSlot` / `Prepare`, and its write thread sends reports to
`127.0.0.1:BBDumpPort` through `Wiimote::Write`. Once linked, `Read` sends them too.
Neither send checks `Common::OfflineReplay`; SFML bypasses the new HTTP/WebSocket/
ENet guards. The file-source setting does not imply the separate no-input flag.

This is reachable during controller initialization, if boot is delayed or never
reaches the session's forced profile: `MainNoGUI.cpp:620` initializes controllers,
whereas `BootManager.cpp:191` applies that profile during boot. It also remains
reachable in a non-session launch. The promise explicitly includes loopback and
failed/missing replay files, so a later game configuration is insufficient.

Gate both UDP dump sites with the offline predicate and add a hardware-free
regression with a fake Wiimote and loopback UDP receiver. Existing offline tests
do not exercise these sites. Finding established by source trace; no real board
was accessed.

### B2 — Windows guest USB keyboard bypasses the no-input switch

**Fixed:** `USB_KBD::Update` returns before key/modifier polling under
`ORCA_NO_LOCAL_INPUT=1`, clears pending input, and `IOCtl` suppresses queued delivery.
The injected keyboard regression checks zero polls and no queued guest report, with
an unset-switch polling/delivery control. Modifier polling shares the same injectable
key reader and retains the original Windows high-bit test.

**Location:** `Source/Core/Core/IOS/USB/USB_KBD.cpp:238`, `:229`, `:283`;
new backend gate at
`Source/Core/InputCommon/ControllerInterface/ControllerInterface.cpp:62`.

**Failure scenario:** On Windows, launch with `ORCA_NO_LOCAL_INPUT=1` outside
`ORCA_SESSION`, enable the emulated Wii USB keyboard, and boot a title that opens
`/dev/usb/kbd`. Zero controller-interface backends does not prevent
`USB_KBD::Update` from polling `GetAsyncKeyState` for keys and modifiers. With
background input allowed, `IOCtl` can deliver those events to the guest. This path
does not use DInput, XInput or WGInput and never consults `LocalInputDisabled`.

Normal Orca sessions mitigate this with forced keyboard settings and determinism;
the new launch switch itself has no session precondition. For the brief's complete
Windows no-local-input guarantee, guard this separate polling/delivery path too,
preserving the unset branch. The new `LocalInput` tests only initialize the
controller interface, GC adapter and real-Wiimote scanner, so they cannot detect
this omission. Static finding; no Windows key polling was performed.

### B3 — A passthrough savestate can restore an active device with no adapter

**Fixed:** Passthrough state loads under `ORCA_NO_LOCAL_INPUT=1` abort before restoring
device activation or endpoints and leave the device inactive. Adapterless update,
control requests and close also return safely. The regression serializes a real active
device with a pending HCI request without opening hardware, then checks rejection and
safe follow-up operations. An unset-switch control checks identical serialized bytes.

**Location:** `Source/Core/Core/IOS/USB/Bluetooth/BTReal.cpp:49`; related
`:339`, `:341`, `:192`, and `:83`.

**Failure scenario:** Save a non-session Wii game with Bluetooth passthrough
enabled and an outstanding HCI request. Relaunch with the same passthrough
configuration and `ORCA_NO_LOCAL_INPUT=1`, then load that state. The new `Open`
return leaves `m_lib_usb_bt_adapter` null. `DoState` still accepts a passthrough
state, restores `Device::m_is_active` and pending endpoints, and the kernel updates
the restored active device. `TryToFillHCIEndpoint` dereferences the missing adapter
at line 192. A subsequent guest control request also dereferences it at line 83.

Reject the incompatible state before restoring the active/endpoints state, or
make the restored device safely unavailable without opening hardware. The
current no-input tests never instantiate this device or restore its state. Normal
Orca sessions force passthrough off, so this is specifically the switch's
non-session/savestate boundary. This is a source-proven null-dereference path,
not a hardware/runtime reproduction.

## Non-blocking

### N1 — Owner relaunch retains diagnostics from the previous lobby

**Location:** `Tools/orca/doubles-rooms.py:157` through `:161`; consumers at
`:201` and `:214`.

The new owner-relaunch branch removes membership/lobby metadata but retains the
room's `frames`, `hashes`, `reports`, and captures. After an old owner reaches
frame 900, leaves, and reopens the code on another compatibility key, a new frame
60 still reports frame 900 because `record` takes the maximum. Old hashes and
reports also remain available to diagnostics. A later gameplay/compatibility run
can therefore be judged using evidence from the preceding lobby. Clear the
room-scoped evidence on reset, or give diagnostics an explicit lobby generation.
The new relaunch self-tests only check metadata and seating, so they miss this.
An additional hardware-free probe reproduced this: admit the old owner, record
frame 900/hash 800 and a report, remove its membership, admit its new lobby, then
record frame 60. Diagnostics retained frame 900, the old hash and the old report.

### N2 — The controller-stream refusal test has a scheduling false negative

**Location:** `Source/UnitTests/Core/Orca/OfflineReplayTest.cpp:106` through `:109`.

The test starts the asynchronous controller stream, checks that no snapshot has
arrived, and immediately stops it before checking the listening socket. Even if
the new startup guard is removed, an unscheduled worker can see `m_done=true`
before its first curl request and exit without connecting. An empty snapshot is
also the ordinary state of a newly started stream. Thus this part can pass with
the protection removed. Assert that no stream/worker was created, using an
observable lifecycle hook, and include a synchronized allowed-mode control.
The synchronous HTTP/WebSocket refusal checks do not share this problem.

## Other review conclusions and validation limits

- The offline predicate matches `Player::FromEnv` / `Player::Poll` source
  classification, including case sensitivity. Unset/empty and lowercase `ws(s)`
  sources retain the previous transport branches. No additional normal-play or
  live-watch networking regression was identified in the reviewed changes.
- The shared backend gate covers Win32's DInput/XInput/WGInput initialization,
  Quartz, XInput2, SDL, evdev and the other compiled controller backends. Empty
  update/refresh/shutdown loops, expression device lookups, mapping-device lists
  and input detection handle missing devices in the inspected code. Native
  Windows/Linux and the Qt Controls UI were not executed in this Mac review.
- No new peer simulation divergence was identified in supported Orca sessions.
  Their forced controller/Bluetooth settings keep the conditional non-session
  findings above out of ordinary paired play. No session protocol, compatibility
  payload or emulation arithmetic changed in this range.
- The 503 hold test drives the real reconnect state machine across the full hold,
  including repeated same-time polls and the deadline. The ranked recording
  regression checks the actual driver caps against the real matched-room parser
  and product-gated writer. Neither claims actual account replay delivery.
- Queue build metadata covers room and queue tickets. Both normal queue caps now
  agree `record` without overriding Auto; explicit casual recording still sends
  `record on`. Worker-log evidence verifies accepted delivery per room, not saved
  playback or completeness of every individual game. The notes correctly leave
  live delivery and the archived-opener integration run unverified.

## Review checks

Required initial base check:

```text
5470249f28 Enable recording capability in queue test flows
BASE-OK
```

`git diff --check b63badef57..HEAD` passed.

All three test commands used
`flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10`, after checking
`uptime` (one-minute load below 20). They waited for the shared lock and ran
serially. No lock was bypassed, removed or broken.

| Check | Result |
| --- | --- |
| `node Tools/orca/queue-e2e.mjs --self-test` | Passed, exit 0; mocked fetch only |
| `doubles-rooms.py` self-tests, invoked through `runpy` in a minimal environment | Passed, exit 0; loopback only |
| Additional owner-relaunch diagnostics probe in the same Python command | Reproduced N1 as described above |
| Existing C++ binary through `Tools/orca/room-fix-tests.py`, with the filter below | 14 passed, 0 skipped/failed; exit 0; 245 ms in Google Test |

```text
OrcaOfflineReplay.*:LocalInput.*:OrcaRoomResilience.ServerClose503RefusalsKeepFriendsUntilTheRoomHoldExpires:OrcaRoomRuntime.RankedRecordingUsesTheMatchedRoomAndAgreedCapability
```

The C++ wrapper supplied its existing minimal environment, temporary user data,
disabled native input and loopback room/keyframe fixtures. The Python harness
command supplied only a fixed PATH and `NO_PROXY=*`. The C++ binary was reused;
this review did not rebuild it or rerun the complete ten-minute suite. These
passing tests do not reproduce or disprove B1–B3; their specific coverage gaps
are identified with each finding.

No builds, installs, external service requests or native input-device operations
were performed during the original review. The original review left the findings
unfixed; the B1–B3 fixes and their subsequent validation are recorded above and below.

## fix-0349 follow-up verification

Required initial base check:

```text
4017359ddd Record review-0349 results
BASE-OK
```

Replacing the three production `.cpp` bodies with their base versions, while retaining
the test access seams, produced three expected failures: the Balance Board regression,
the guest keyboard regression, and the passthrough-state regression. The fixed sources
were restored and rebuilt. The final UDP test also waits up to 50 ms for delivery before
asserting silence, and checks the complete normal/live-source datagrams byte for byte.

The keyboard test injects the key reader while running the real `Update` and `IOCtl`
paths. Native Windows execution was unavailable on this macOS host. All device tests
use temporary user/NAND data and synthetic input or IPC requests; no physical board,
keyboard polling, or Bluetooth adapter was accessed.

All build, test, and warning checks used
`flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10`, after checking that
one-minute load was below 20. Builds used at most six jobs; Google Test ran serially.
The shared lock was allowed to wait normally and was never removed or bypassed.

| Check | Result |
| --- | --- |
| `cmake --build build-orca-tests --target tests --parallel 6` | Passed |
| Unfiltered `build-orca-tests/Binaries/Tests/tests`, launched with a minimal environment and temporary user data | 2,116 passed, 94 skipped, 0 failures; exit 0; 67,322 ms |
| `python3 Tools/orca/room-fix-tests.py build-orca-tests/Binaries/Tests/tests` (unfiltered) | 2,187 passed, 23 skipped, 0 failures; exit 0; 602,661 ms |
| All five touched C++ translation units, using their compilation-database commands with `-Werror -fsyntax-only` | Passed, exit 0; includes all three touched production headers |
| `git diff --check` | Passed |

All four new tests passed in both unfiltered runs. The wrapper supplied the 71 room/
keyframe fixture cases skipped by the direct run. The remaining 23 skips require
opt-in live services, bridges, disc images, or design captures; these were not enabled.
No required B1–B3 gate remains blocked. No installs or pushes were performed.
