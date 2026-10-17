# train-0350 verification

Worktree: `/Volumes/YGWorktrees/ClaudeWorktrees/orca-0348`, branch `train-0350`.
Local commits only. No Dolphin process was launched.

## Base

The first command, before edits, returned:

```text
ce0a4d906a Set version 0.3.49 and player notes
BASE-OK
```

The check was `git merge-base --is-ancestor ce0a4d906a HEAD && echo BASE-OK`.

## Fullscreen feedback and host controls

Read the supplied `delta-overlay-0350-contract.md`. The bridge-gated native overlay draws
Δ beside the YouGame orb, including when no orb placement has arrived yet. Click the orb
for Feedback, Volume up, Volume down, Mute, Exit fullscreen, Leave, and Controls.
Up + Start opens that native menu through the existing corner shortcut; D-pad navigates,
A selects, and B closes. F8 requests feedback. Native window keyboard events also support
Shift+Tab and arrow/Enter/Escape navigation. On macOS the embedded window remains non-key;
F8 is checked only while the app gives the visible game focus. The stdin commands `feedback`
and `host-menu` provide the same entry points for a bridge key relay.

The capture uses the existing XFB frame dumper. It writes `feedback-<steady-time>.png.tmp`
inside `File::GetUserPath(D_USER_IDX)` (the `-u` directory). PNG saving now checks file close
success. Only a successful save followed by rename publishes the bare final PNG name and
then emits `orca feedback <name>.png`. Pending captures cannot overlap; the five-second
cooldown starts after completion/output. Failed capture, missing file, oversized file, or
failed rename emits no feedback line. A stopping session emits no host controls. Host actions
use the five contract strings and a rolling limit of ten per second. Controls uses `orca orb`.

The overlay reads controller navigation; its returned gameplay pad bytes and the existing
shortcut mask are unchanged. No rollback, game rules, simulation, or guest writer was changed.
Tests cover menu navigation/held buttons, exact host lines, pending/completed capture timing,
host rate limits, actual PNG signature, publication order, failed capture, and filename limits.

## Harness reports

`YG_INPUT` being nonempty or `ORCA_TEST_COMMANDS=1` identifies a scripted input driver.
The stdio test-driver opt-in marks its session before the first pad snapshot, so its initial
hello is marked too. Room begin, game-start, game-report, casual finish, final finish/retry,
and app result JSON (the app's lobby_result/set path) gain `"harness":true`. Both the app SDK
hello and peer hello gain the field. Normal messages bypass modification and retain their
original serialization.

Tests cover both opt-ins and absence, including byte equality; the actual peer-hello path;
and loopback room begin/game-start/game-report/finish. The existing legacy hello golden
continues checking the complete normal wire bytes.

## Builtin writer audit

Audited every direct `GuestMemory::Write8/16/32` call and every `Writer::U8/U16/U32` call in
`Source/Core/Core/Orca/Games/Brawl`. Also checked for raw HostWrite/CopyToEmu/memcpy writes; the only memcpy copies a host float.
The complete 161-site inventory is
[train-0350-writers.txt](train-0350-writers.txt). Brawl's builtin variants share
`BrawlGame::Regions()`. The generic module adds no guest-memory write sites.

The writer of `0x805b8e24` is the scene-manager exit-code writer: `FriendsMove::Apply`
(`OnlineMenu.cpp:459`) and `FreshMove::Apply` (`OnlineMenu.cpp:511`). They dereference
`0x805A0060` to the singleton `0x805B8BA0`, then write offset `0x284`; both also write
its step at offset `0x288` (`0x805B8E28`).

| Writers | Declaration |
| --- | --- |
| `FriendsMove::Apply`, `FreshMove::Apply`, exit and step | Added exactly `[0x805B8E24, 0x805B8E2C)` |
| `Rules::ForceCodeMenu`, restored game pause word | Added exactly `[0x805B8A08, 0x805B8A0C)` |
| `RankedPPlus::ClearStaleMusicSelect`, ten cleanup words | Added `[0x80002810, 0x80002814)`, `[0x80002818, 0x8000282C)`, `[0x80002830, 0x80002840)` |
| P+ stage preset, page index, strike-table writers | Existing `pplus-stage-data` |
| Code Menu defaults/state/activation | Existing `pplus-code-menu`; the range assertion covers every default |
| `FreshMove::ApplySeed` | Existing `rng-default`, `rng-menu` |
| MatchBlock, SetBlock, Queue, CharOrder, StageCursors, BrawlStages state/latches, rules header/saved data/friends bytes | Existing `match-block` scratch block |
| Rules/RankedSet/Results rules, picks, records, tags/controls | Existing game heaps, primarily MEM2 |
| OnlineSeats tasks/areas/models; BrawlStages and RankedPPlus tasks/cursors; NativeText objects/buffers; MenuText messages; Relabel/CssTitle archive textures/palettes | Existing pointer-backed game heaps |

The five new ranges total 52 bytes. Existing ranges were not widened. My Music's holes at
`+0x04` and `+0x1C`, adjacent pause/manager words, and cross-boundary writes remain outside.
Tests record actual open-Code-Menu, scene-exit, and all ten My Music cleanup writes and check
the declarations; a separate boundary test checks the holes and adjacent words. Synthetic
fixture addresses are not used to justify expanding the game heaps.

## Gates

All builds and suites use the shared `flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10`
wrapper, after checking `uptime`. Loads were below 20. Builds and compiler checks use at most
two workers; tests run in a single process. Build output and gate logs live in `build-orca-tests`.

| Gate | Result | Log |
| --- | --- | --- |
| `cmake --build build-orca-tests --target tests dolphin-nogui --parallel 2` | PASS, exit 0 after the test filter correction | `train-0350-rebuild.log` |
| `python3 notes/train-0350-werror.py` (real flags, object compiles, two workers) | PASS, 19/19 macOS translation units; updated RoomTest recompiled cleanly, exit 0 | `train-0350-werror.log`, `train-0350-werror-room.log` |
| Full unit binary, minimal environment | PASS: 2,125 passed, 95 skipped, 0 failed; 2,220 total; exit 0 | `train-0350-tests.log` |
| Focused feature/region regressions through the loopback runner | PASS: 19/19, exit 0 | `train-0350-focused-tests.log` |
| Full unit binary through `Tools/orca/room-fix-tests.py` | PASS: 2,197 passed, 23 skipped, 0 failed; 2,220 total; exit 0 | `train-0350-room-tests.log` |

An initial full run found an assertion in the new hello test's inspection of a non-payload
control message. The test now filters for an object payload before looking for `hello`.
The rebuilt binary passed the focused regressions and both final full runs. The initial log
is retained as `train-0350-tests-initial.log`.

The direct unit run uses a minimal environment with `ORCA_NO_LOCAL_INPUT=1` and all six SDL
isolation variables. The room runner supplies the same isolation and only loopback fixture
endpoints. Neither run inherits live-test opt-ins or app credentials. The remaining fixture-run skips
are opt-in live, disc, latency, and visual checks. Full direct runtime: 65.393 s; fixture runtime:
601.495 s. The fixture run adds 72 passing tests over the direct run. `git diff --check` passed.
The generated `imgui.ini` from the test suite was removed.

No remaining implementation work or blocked gate on this macOS build. All changes remain local.


The native macOS event handlers are compiled in this build. Windows' input wiring is excluded
from the macOS compilation database and receives source review only here. No GUI/fullscreen
manual run is part of this scripted headless verification.
