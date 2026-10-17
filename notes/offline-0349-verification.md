# offline-0349 verification

Worktree: `/Volumes/YGWorktrees/ClaudeWorktrees/orca-0348`, branch `train-0349`.
Initial base check, before edits:

```text
dc9f2c3b1f Test the 503 hold, record casual queue rooms, two-version friends fixture
BASE-OK
```

`git merge-base --is-ancestor b63badef57 HEAD` exited 0. The pre-existing
untracked `imgui.ini` and `notes/tests-0349-runs/` are outside this change.

## Playback policy

There is one predicate, `Common::OfflineReplay()` in `Common/Network.cpp`, also
available as `Rollback::ReplayStream::OfflineReplay()`. Common owns it so its
transport implementations do not depend on Core. It reads only
`ORCA_REPLAY_SOURCE`: a nonempty value without the exact `ws://` or `wss://`
prefix disables networking. This matches the player's source classification.
It does not depend on whether the file exists or opens successfully. Unset,
empty and live watch sources take the previous branches, without new logging,
configuration changes, URLs, payloads, ticket formats or protocol changes.

After log initialization, `UICommon::Init` emits this line once for file playback:

```text
replay offline (file source): network off
```

## Network path inventory

Audited curl performs, HttpRequest/WebSocket users, socket creation, connects,
ENet sends, discovery and network workers in Common and Orca, and followed their
callers and other startup paths through Core, InputCommon and UICommon.

| Path | File replay gate |
| --- | --- |
| Direct-link options, worker, initial firewall UDP bind, DNS, libjuice host/STUN/TURN gathering, allocations, peer sends and link retries | `DirectOptionsFromEnv` returns no options; `DirectLink::Start` returns false before setting started or creating its worker. Room `StartDirect` also returns. |
| Orca room initial ticket, bridge ticket, room WebSocket, reconnect worker, pings, peer/replay messages and result/net statistics reports | `Online::StartLocked` retains its existing product-mode refusal. Direct `YouGameRoom::Impl` construction ends synchronously before its worker. No room/reconnect/report worker or transport becomes active. |
| Room ticket refresh, keyframe-store ticket refresh | `YouGameRoom::FreshTicket` refuses before cached-ticket access or any bridge/HTTP request. |
| Room bridge command posts, status/report posts, mirror posts and mirror end | Room construction prevents their worker; `BridgeLoop` independently returns; shared HTTP refuses every fetch. |
| Room bridge SSE events (raw curl) | Room construction prevents startup; `EventsLoop` independently returns before curl initialization. |
| HTTP controller bridge SSE events (raw curl), including reconnect | `StartControllerStream` creates no stream or worker. Stdio controller input remains available. |
| HTTP keyframe upload, download, deletion and retries (raw curl) | `WithRetries` refuses before ticket callbacks, `Delete` returns before tickets/worker creation, and raw `Request` independently refuses before curl initialization. Local file stores remain local. |
| Replay watch socket and development watch-ticket HTTP request | File playback never enters the watch branch; shared WebSocket and HTTP also refuse under file mode. Product ws(s) origin validation and live watch behavior remain as before. |
| Shared HTTP GET, both POST overloads and multipart POST, redirects | `HttpRequest::Impl::Fetch` returns no response before curl setup/perform, even for a client constructed before file mode. This covers all callers below as well as Orca's HTTP paths. |
| Shared WebSocket connect/upgrade and subsequent sends, receives, close frames | `WebSocket::Connect` returns false before curl creation, leaving no open transport. |
| Dolphin telemetry and HTTP analytics/report uploads | `DolphinAnalytics::ReloadConfig` installs no HTTP backend; `HttpAnalyticsBackend::Send` independently refuses; shared HTTP is also gated. |
| Update checks, update test acknowledgment, external updater launch | `AutoUpdateChecker::CheckForUpdate` returns before cleanup/request; `TriggerUpdate` returns before launching the updater. Shared HTTP also refuses. |
| Classic NetPlay direct client/server | Both constructors return before ENet host/socket creation. |
| Common traversal startup, server DNS, retry/hole-punch/test/ack packets | `EnsureTraversalClient` returns false before creating an ENet host; `ReconnectToServer`, `NewTraversalTest`, `ResendPacket` and `HandleServerPacket` also return. |
| Common ENet wakeup datagram and reliable packet sends | `WakeupThread` and `SendPacket` return before sending. |
| Common UPnP discovery, gateway description requests, mapping and unmapping | `TryPortmapping` and `StopPortmapping` return before worker creation. |
| Wii guest sockets, DNS, ICMP, sends and SSL through IP | Both `NetIPTopDevice::IOCtl` and `IOCtlV` refuse regardless of determinism initialization. `WiiSockMan::NewSocket` also refuses before socket creation. |
| WiiConnect24 scheduled mail/download and guest requests | File mode prevents the scheduler thread and makes `RefuseInSession` refuse requests. HTTP requests additionally hit the shared gate. |
| GameCube BBA built-in TCP/UDP/DNS/UPnP, XLink, TAP and IPC backends | `CEXIETHERNET` skips backend activation, receive startup and sends. Shared `TAPServerConnection::Activate` also refuses, covering both BBA and modem connections. |
| Triforce/AM media-board guest sockets and restored socket state | `socket_` refuses; state loading skips socket recreation. |
| DualShock UDP controller discovery, configured-server DNS, subscriptions and input | `ConfigChanged`, `StartHotplugThread` and device `UpdateInput` return before resolving or sending. |
| Discord presence IPC, initialization and updates | Existing setting guards also check file mode before RPC calls. |
| GBA link and USBGecko TCP listeners | Their connection workers return before listener/socket creation. |
| GDB TCP/local debug listener | `Init` and `InitLocal` return before socket creation, unlink or accept. |
| MemoryWatcher local datagram socket | `OpenSocket` returns false before socket creation. |
| Game banners, NetPlay index, Gecko downloads, Wii updates/downloads, achievement HTTP, disc-verifier Datel data | All use the gated `Common::HttpRequest`; no additional transport. |
| Standalone `Common/TraversalServer.cpp` | Separate server program with its own `main`; not linked into YG Dolphin. |
| `UpdaterCommon` / platform updater download code | Separate updater executable; file mode prevents `TriggerUpdate` from launching it. |
| Bluetooth/USB hardware I/O, stdio app bridge | Hardware/local pipes, no IP transport. Stdio remains available for playback controls and status. |

No external service requests, credential reads, app instances, downloads, signing,
pushes, releases or deployments are part of this verification.

## Tests and gates

New `OrcaOfflineReplay` tests cover source classification (including a missing
file), all shared HTTP request variants, WebSocket connection refusal, telemetry,
controller bridge startup, direct-link startup and restart eligibility for a live
watch, direct room construction, room-ticket refresh, the Online launcher,
keyframe upload/download/delete before ticket callbacks, classic NetPlay,
traversal startup, ENet sends, TAP/modem connection activation, debug-listener startup and update checks. A local listening TCP socket proves that
the HTTP/WebSocket/telemetry/controller calls did not connect. The existing
full suite exercises ordinary/unset-mode networking through local fixtures.

All heavy commands run under the shared `flock` and `nice -n 10`; build concurrency
is two, and the test binary runs serially. Load is checked before each job.
Evidence is kept in the ignored `build-orca-tests/offline-0349-artifacts/`.

| Gate | Result | Evidence |
| --- | --- | --- |
| `-Werror` syntax check using each touched source's active compilation command, two workers | 29 translation units passed, no warnings/errors | `werror.log` |
| Build `tests` and `dolphin-nogui`, two jobs | Passed | `build.log` |
| Compile touched translation units into objects with `-Werror`, two workers | 29 translation units passed, no warnings/errors | `werror-build.log` |
| Full test binary directly, isolated environment and temporary working directory | 2,201 tests; 2,108 passed, 93 skipped, **0 failures**; 68.231 s | `full-direct.log`, `full-direct.xml` |
| Full test binary through `Tools/orca/room-fix-tests.py`, local room/keyframe fixtures | 2,201 tests; 2,178 passed, 23 skipped, **0 failures**; 602.243 s | `full-room-fix.log`, `full-room-fix.xml` |

The syntax check strips dependency/output arguments and the compiler cache launcher
from `compile_commands.json`, then appends `-Werror -fsyntax-only`; it checks the
new unit-test source as well. The object check uses the same active compilation
commands with `-Werror -c`, writing separate validation objects under the artifact
directory. Both checks completed successfully. The isolated runner supplies the six SDL switches,
`ORCA_NO_LOCAL_INPUT=1`, a temporary `TMPDIR`, a loopback updater URL and, only for
the fixture run, the wrapper's two loopback fixture URLs. It supplies no app
credentials, live-test opt-ins or replay source to the full suite.

All eight new offline-replay tests passed in both full runs. The direct run skips
the 70 room/keyframe fixture tests as well as the 23 existing live/disc/probe
opt-ins. The fixture run executes those 70 local tests and skips only the 23
opt-ins. Both runs use the entire test binary, with no filter, and exit 0.

`git diff --check` passed. All requested gates are complete; no task blockers
remain. The change is committed locally only.
