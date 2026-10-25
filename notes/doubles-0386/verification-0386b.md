# doubles-0386b verification

Base check before edits on branch `doubles-0386`:

```text
29c4b71909 Include local seats in room tickets
BASE-OK
```

`git merge-base --is-ancestor 29c4b71909 HEAD` passed. No version bump.

## Capability contract

`3166180cc5` appends `qdoubles` after optional `coseats` in
`Source/Core/Core/Orca/Status.cpp:209`. It means the engine plays casual queue doubles rooms.
There is no `pick-doubles` offer. With `ORCA_COSEATS=0`, only `coseats` disappears:
one seat per computer still supports doubles.

Exact offers for RSBE01, PPLUS32 and PPLUS321 with co-seats enabled:

Session (23 capability words):

```text
orca caps join leave stats pause delay perf direct host chat yougame gfx music results locks queue2 pick-casual pick-ranked talk rumble record hostui coseats qdoubles
```

Product playback (22 capability words):

```text
orca caps leave stats pause delay perf chat yougame gfx music results locks queue2 pick-casual pick-ranked talk rumble replay broadcast record hostui coseats qdoubles
```

The legacy reader's 24-word budget counts capabilities after the `orca caps` prefix.
With co-seats disabled the counts are 22 and 21 respectively.

Before the implementation, both
`OrcaStatus.CoSeatsCapsAreAppendOnlyAndFitLegacyReaders` and
`OrcaUXHostUI.WithoutAgreementTheOrbAndShortcutKeepTheirLegacyPaths` failed against the
unchanged engine. After the implementation both passed in the full suite. The Status test
pins both printed lines, exact counts, all three profiles, co-seats-off offers, and negotiation
without offering `pick-doubles`.

## Room handoff

`room-contract.md` answers #15553 from engine source, with file:line citations and source
fragments. `test_room_contract.py` checks the four requested answers and verifies cited fragments
against the current source lines. Before adding the document it failed with
`missing doubles-lead room contract`; afterward its one test passed.

## Gates

- Mac `cmake --build build-doubles-0386 --target tests dolphin-nogui --parallel 2`: passed.
  The existing cache matched this worktree and was usable.
- Full `*Orca*:*Broadcast*` suite: **1,490 passed, 23 opt-in skips, 0 failures/errors**,
  1,513 cases in 150 suites. Loopback room/keyframe fixtures were enabled, live opt-ins unset.
  The required real-disc boots ran separately for the three profiles below.
- Document coverage/citations: **1 passed**. `git diff --check`: passed.
- keyCode: **`f86645e20656efe5`**, unchanged against the run 1 baseline. This is the key-function
  fingerprint; the separate legacy engine-commit component follows the commit.
- Fresh runs of the retained `dolphin-emu-nogui-baseline-6ad37fe3e1` reproduced run 1's baseline
  hashes exactly. Current RSBE01, PPLUS32 and PPLUS321 each exited 0 and compared **601 hashes,
  frames 0–600, 0 differences** against that fresh baseline. The caps line is status output,
  outside the frame hashes. No differences or exclusions needed explanation.

All builds, suites and game runs used the requested full `heavy-slot.sh` path with
`--pool heavy --prio 2 --label orca:codex-doubles-0386b`, then the shared `flock` and `nice -n 10`.
Load was below 20 at admission. Builds used two workers and the suite one. Every game run used
`-v Null -C "Dolphin.DSP.Backend=No Audio Output"`, disabled SD writes and external network,
and kept generated user/cache files in the worktree. No queue restart was needed.

Evidence is retained in ignored `build-doubles-0386/gates-0386b/`:
`caps-before.{log,xml}`, `contract-before.log`, `contract-after.log`, `build-after.log`,
`full.{log,xml}`, `full-summary.json`, `keyCode.txt`, `baseline-hashlogs.log`,
`final-hashlogs.log` and the six profile hashlogs.

No blockers or remaining implementation work.
