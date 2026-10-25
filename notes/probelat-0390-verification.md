TASK probelat-0390, local engine investigation, 2026-10-25.

Started with `git checkout -b probelat-0390 origin/train-0389`. Required base check:

```text
7d352ce65a YG Dolphin 0.3.89
BASE-OK
```

The demonstrated delay source is the room loop's dependence on the status-output mutex.
At 7d352ce65a, `Source/Core/Core/Orca/Status.cpp:279-282` reads agreed capabilities under
`s_mutex`; `Shaders` holds that mutex while `Print` synchronously calls `fputs` and `fflush`
(`Status.cpp:293-298`, `Status.cpp:66-71`). The room calls this accessor from
`MirrorNeedsPublish` (`YouGameRoom.cpp:3111` on the base, now 3160), at the beginning of `Tick`,
and from `DirectAllowed` (now 2108). A slow output consumer can consequently prevent the next
socket read, despite `OnProbe` itself having no emulator lock. The regression test enters a
controlled stream write callback, holds it there, and checks that `Cap("direct")` completes
while that write remains blocked. It failed on the original code; the new capability mutex
is held only while publishing/reading the agreement, never during output. Capability
withdrawal, reset and negotiation behavior remain covered by the existing status tests.

This is a reproduced engine-side blocking path and the strongest code-supported candidate,
not a conclusive attribution of the two supplied Legion events. No Linux process or live room
was contacted here. The Mac/Linux scheduling difference remains: `ThreadPriority.cpp:74`
assigns interactive QoS on macOS, while Linux has no branch. There is insufficient evidence
to change scheduling policy.

The transport audit found no platform-specific one-second read or flush tick. Both package
scripts select bundled dependencies. `Common/WebSocket.cpp:39` uses `poll` on macOS and Linux;
the room requests 2 ms (`YouGameRoom.cpp:2997`). `ReceiveText` tries `curl_ws_recv` before
polling (`WebSocket.cpp:294`), so buffered TLS/WebSocket data is consumed without a new socket
readiness event. `SendText` calls `curl_ws_send` immediately (`WebSocket.cpp:258`) and waits
for writability only on `CURLE_AGAIN`. The bundled curl already defaults `tcp_nodelay` to true
(`Externals/curl/curl/lib/url.c:469`). Its `ws_flush` drains the send buffer before reporting
completion (`Externals/curl/curl/lib/ws.c:1233`). Direct-link DNS and candidate gathering run
on the link worker, outside its command mutex (`DirectLink.cpp:1314`, `:1390`, `:1725`).
Ordinary room writes can still wait up to their existing two-second send timeout; that is
another delay the new read-gap measurement can distinguish. The 400 ms probe gap rule drops
a sample rather than holding it for a later tick; its traffic limits are unchanged.

`b104f8881d` adds one NETPLAY notice for each well-formed queue probe, with an at-most-eight
character sanitized prefix (a nonce of eight characters or fewer prints `-` rather than its
full value). Example from the injected-clock QA test:

```text
orca probe n=12345678 recv→ack 2000000 us write-flush 0 us read-gap 0 us read 0 us result=sent
```

`recv→ack` starts at return from the socket receive, before JSON parsing or QA delay, and ends
after the ack's synchronous send completes. `write-flush` is time inside `SendText`, including
libcurl's flush/writable waits, not proof of remote delivery. `read-gap` is time spent outside
`ReceiveText` since its previous return; `read` is time in that receive call. Both include
scheduler delay in the measured interval. QA queues retain the receipt through both inbound
and outbound holds. Skipped probes have both ack/write timings `-1`, with `result=gap`,
`duplicate`, `expired`, `budget` or `stopped`; a failed send reports `send-failed`. The log
runs after the send through the normal diagnostic logger, without the app status mutex.

`2f0d34c13a` separates capability reads from synchronous status output. The controlled
blocked-output regression failed before the fix and passes after it.

Validation artifacts are in ignored `build-probelat/evidence/`. A fresh Release build was
configured with NOGUI/tests enabled, Qt/autoupdate/app-bundle disabled, and compiler launchers
disabled; no stale cache was reused. All builds and suites used the prescribed heavy-slot
wrapper, shared flock and nice 10, with two build workers and load below 20 at submission.
The original engine implementation was built first and retained as
`build-probelat/Binaries/dolphin-emu-nogui-baseline-7d352ce65a`; only the added unit tests and
fixture snapshot field differed at that point (see `baseline-provenance.txt`).

- `before.xml`: all three new tests fail on the original implementation.
- `measurement.xml`: all 13 probe tests pass after the measurement commit.
- `focused.xml`: all 20 status/probe tests pass after the lock fix.
- `build-committed.log`: final committed Mac `tests` and `dolphin-nogui` build passed.
- `full.xml`: 1,557 cases in 155 suites; 1,534 passed, 23 opt-in skips, zero failures/errors
  (697.252 seconds), filter `*Orca*:*Broadcast*` through the loopback fixture wrapper.
- `keyCode.txt`: baseline and final both `f86645e20656efe5`.
- `hashlogs-baseline.log` and `hashlogs-final.log`: RSBE01, PPLUS32 and PPLUS321
  each exited 0 on both binaries, with all 601 frames 0-600 present and zero differences.
  All six runs used `-v Null`, `-C "Dolphin.DSP.Backend=No Audio Output"`, isolated user
  directories and a process sandbox denying network access.

Linux-lead confirmation on the Legion is still required. Build this branch's engine and tests
with the normal Linux prerequisites, without a release/package/signing step. Run
`python3 Tools/orca/room-fix-tests.py <linux-build>/Binaries/Tests/tests
--gtest_filter=OrcaStatus.*:OrcaRoomPingRange.*` (quote the filter in the shell).
Launch the new engine through the existing YouGame bubblewrap+pasta launcher, preserve the
jail unchanged, and keep `-C "Dolphin.DSP.Backend=No Audio Output"`. Compare measurement-only commit `b104f8881d` with fixed commit `2f0d34c13a`, using matching
engine commits on the Mac and Legion in each case. Enable NETPLAY notice logging and reproduce
several ranked pairs during direct negotiation/game loading. Collect only the new probe lines and correlate
the eight-character prefixes with server-timed ping_check samples. A large read-gap with low ack/write time on the measurement-only build, disappearing on the
fixed build together with the server RTT spike/false too_far, supports this attribution. If server RTT remains
high, large read-gap points to intervening engine work; large read/write time points to the
receive/send path or scheduling. If all local times are small, the delay is before the local
receive or after local write completion; these logs alone cannot locate it further.

Version remains 0.3.89. No push, release, signing or deployment was performed.
