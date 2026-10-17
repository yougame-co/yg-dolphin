# rankedrec-0349 verification

Worktree: `/Volumes/YGWorktrees/ClaudeWorktrees/orca-0348`; branch: `train-0349`.
Baseline: `eefd41b053 Add local input configuration and queue build metadata`.
Required ancestor check: `BASE-OK`.

## Investigation

The normal queue-e2e q2 command was
`caps join leave stats pause delay perf direct host results queue2`.
The q1 command was the same without `queue2`. Neither agreed `record`.
`PAGE_CAPS` already listed `record`, but the normal paths did not use that list.
`pageCommands` added the capability only when `--record` also requested `record on`.
Consequently the explicit-on comparison changed both capability agreement and choice.
Later `caps` commands replace the complete agreement (`Status::SetAppCaps`), so a
previous agreement does not survive an answer that omits `record`.

The room path is `HostInPlay` / `JoinInPlay` -> `ShutdownInBackground` ->
`StartRoom(matched_code, ..., false)` -> `s_room`. `Online::Room()` returns that
shared pointer. The welcome parser assigns its `room.queue` string to `ranked`
before logging `a matchmade ranked room`; `queue2` is a capability/header flag,
not a queue name. `ReplayStream::Writer::Pump` calls `RecordWanted`, which requires
both the agreed record capability and that matched room. No engine policy change
is justified by this trace.

## Runtime proof before the correction

The new C++ regression failed before the capability correction. It used the real
`Online::StartRoom` welcome parser and the product-gated `ReplayStream::Writer`.
The control agreed `record` without sending `record on`; the failing case used
the exact q1/q2 capability constants read from `queue-e2e.mjs`.

Measured q2 host values at the writer decision:

```text
record proof: q2 code=record20 seat=0 queue=ranked cap=true auto=true stream=true
queue driver proof: q2 code=record20 queue=ranked cap=false auto=false
```

The joiner also returned matched code `record21`, queue `ranked`, and auto=true
when the cap was agreed, versus auto=false with the driver's old caps. The first
fixture additionally rejected that joiner's fake peer because its fixed hello
compatibility was `test`; the fixture now accepts a per-room compatibility value
so the final test checks a healthy stream on both seats. Its existing default
remains unchanged for other tests.

The independent dry driver regression failed with
`default queue caps must agree record`.

This proves the provided default queue-e2e reproduction omitted the capability;
it does not reproduce an engine failure when that capability really is agreed.
In particular, the brief's agreed-cap premise does not match the normal checked-in
driver command. The original `Online::RecordWanted` policy already implements the
requested ranked default and explicit-off precedence, so it is left intact.

## Correction and coverage

Both normal queue flows now agree `record` and keep the `auto` choice. All q1
scenario commands use the same constant; q2 keeps its existing queue2 flag.
`--record` remains the explicit casual opt-in and still sends `record on`.
The C++ regression uses those actual driver constants, replaces a solo room with
the matched room, and exercises q1/q2 as host and joiner. It also checks missing
capability, explicit off, and casual/friends default-off and explicit-on behavior
through the same decision and gated writer.

The final loopback run passed the regression (233 ms). Its q2 evidence was:

```text
record proof: q2 code=record20 seat=0 queue=ranked cap=true auto=true stream=true
queue driver proof: q2 code=record20 queue=ranked cap=true auto=true
record proof: q2 code=record21 seat=1 queue=ranked cap=true auto=true stream=true
queue driver proof: q2 code=record21 queue=ranked cap=true auto=true
```

The q1 host/joiner values matched: ranked, cap=true, auto=true, stream=true.
The casual, friends, missing-cap and explicit-off controls also passed.

## Operator reproduction (not run here)

For an authorized staging run, set `ORCA_BUILD=<version>:<commit>` for the binary,
then run the existing driver with the operator's usual listing/test-session
arguments and staging URL:

```sh
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 node Tools/orca/queue-e2e.mjs <nogui> <disc> --site <staging-url> --q2 --queue ranked --keep
```

Do **not** add `--record`, `--replay`, or `--replay-room`, or inherit the replay
writer override variables: this must exercise the product writer on `auto`.
Check each `caps` answer includes `record`, no `record on` is sent, and the Orca
log prints `Replay: streaming history ... to the room from frame ...` after the
match starts. Omit `--q2` for the q1 control. An explicit `record off` from the
page must suppress recording; casual and friends require `record on`.
The driver starts at most two headless Orcas and supplies the six SDL variables.
Use the listing's established signed-in test setup to inspect saved replays;
anonymous dev tickets do not provide account replay-delivery evidence.

No live service was contacted, and the outside-worktree staging logs were not
opened. Live staging replay delivery remains an operator follow-up under the
brief's no-network constraint.

## Gates

- Final `cmake --build build-orca-tests --target tests --parallel 2`: passed.
- `node Tools/orca/queue-e2e.mjs --self-test`: failed before the correction;
  passed afterward, including the q1/q2 capability/no-record-on regression.
- Touched `RoomTest.cpp`: the build's exact compile command with
  `-Werror -fsyntax-only` passed with no diagnostics.
- Full binary directly, with a minimal environment and local input disabled:
  2,206 tests, 2,112 passed, 94 fixture/live/asset opt-in skips, **0 failures**
  (65.953 seconds).
- Full binary via `python3 Tools/orca/room-fix-tests.py`:
  2,206 tests, 2,183 passed, 23 live/asset/opt-in skips, **0 failures**
  (602.775 seconds). The new matched-room recording regression passed for
  q1/q2 host/joiner and all explicit-choice/casual/friends controls.

Every build/check/suite ran under
`flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10`; builds used two jobs,
and tests one worker. A brief load spike above 20 was waited out before the
full-suite commands were started. No lock was deleted or bypassed.



Raw local evidence is retained in the ignored build directory
`build-orca-tests/rankedrec-0349-logs/`:

- `before-recording-runtime.log`: failing C++ recording-decision regression.
- `before-queue-driver.log` / `after-queue-driver.log`: dry regression before/after.
- `final-build.log` / `werror.log`: successful compile gates.
- `full-direct.log` / `full-loopback.log`: complete final suite output.

Only the capability correction, tests/fixture, and these verification notes are
committed. No push, deployment, or external-service operation was performed.
