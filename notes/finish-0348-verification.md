# Finish 0.3.48 verification

Task `finish-0348`, 2026-10-16, branch `train-0348`. **Incomplete: stopped on
the required machine-load gate.** No push, external service, production action,
signing, mailbox or other MCP call was made.

## Required base check

Before any edits:

```text
85112b1bd6 Handle room closes and reconnect refusals
BASE-OK
```

`git merge-base --is-ancestor 85112b1bd6 HEAD` succeeded, and the worktree was clean.

## Changes

- ORCA.md 0.3.48 player notes describe the sitting-out action and the three
  player-visible changes from the required base. The older four-player-room
  announcement remains under its original 0.3.46 release.
- Current compatibility text states engine 31, Brawl/Project+ game revision 1
  from 0.3.48, results-only/GenericModule revision 1 and legacy launcher 0.
  Other `ux=` references were checked; the older numbered entries are history.
- Friendlies case 4c explicitly proves seat retention after the fixture closes
  a socket whose send blocked for one second, followed by reconnect.
- The existing replay golden now asserts the 1,516-byte chunked sample's SHA-256
  `36d2a4e821fd27739e20fcd4726b42be65d1c3420b2e7a47bc47ea79f2222c7e`.
  Its comment requires updating the YouGame rooms `streams.chunked` fixture too.
- The doubles fixture accepts a legacy `--pplus` launcher and explicitly disables
  local input for every peer. A launcher-command regression and separate Project+
  controls/reproducer are included, but the new regression has not yet run.

## Results and blocker

| Check | Result |
| --- | --- |
| Initial tests + NoGUI incremental build, two jobs | PASS; [build log](finish-0348-build.log). The later explicit `<string_view>` include has not been rebuilt. |
| Project+ case 2, first attempt | FAIL scripted menu navigation; retained evidence, exact own PID stopped and cleaned. |
| Project+ case 2, adjusted controls | INCOMPLETE: first fight and panel-empty action observed on all three peers, matching hashes through 4140; machine load stopped the run before fight 2. |
| Project+ case 1, separate run | NOT RUN. |
| Final full test binary | NOT RUN for this task. Earlier train/base logs are not a substitute for this gate. |
| Touched C++ actual `-Werror` compile | NOT RUN for this task. |
| Updated Python runner regressions / self-test | NOT RUN for this task. |
| `git diff --check` | PASS. |

The Project+ runner detected load >=18 and sent SIGINT to its own fixture PID
79524; the fixture's `finally` reaped only its children. Post-stop load was
19.05. Both per-run writable launcher/SD clones were removed; the shared source
and read-only disc were never changed. No further heavy job was started, per
the explicit stop-on-block rule.

See the [Project+ evidence](friendlies-0348-verification.md#project) for attempts,
controls and commands. Remaining work: finish Project+ cases 2 and 1, then run
the final gates via `python3 notes/finish-0348-gates.py`. That reproducer wraps
each build/test command separately in the shared lock and nice 10, disables
ccache writes, uses two build jobs and runs the full binary through the existing
loopback-only fixture wrapper with the authorized disc. It has not been executed.
