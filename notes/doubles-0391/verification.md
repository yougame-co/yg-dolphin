# rdoubles-0391 verification

Engine repo only, branch `rdoubles-0391`, no version bump, no push.

Base check before changes:

```text
dd314fb40a YG Dolphin 0.3.90
BASE-OK
```

The contract was committed first, by itself, as `4042fd720a`. The final
[result contract](result-contract.md) has pinned live engine source citations and names the
rooms/site changes needed from doubles-lead. Service references were read from the local
YouGame `origin/claude/v2` at `7b51c2e868`.

## Commits and failing-before checks

| Commit | Change | Failing-before check |
| --- | --- | --- |
| `4042fd720a` | Contract first | Document-presence/coverage assertion: missing contract |
| `b86143fff2` | Native Brawl/P+ team results | `OrcaResults.FourSeatJointWinnersKeepSeatOrderedDetail`; `FourSeatResultsWaitForConfirmationAndRollbackReplacesTheWinner` |
| `9e096f127d` | Four-account ranked admission | `OrcaRoomRankedDoubles.FourSingleSeatAccountsAreAdmittedAndInvalidMapsRefuseOnce` |
| `77d4a6eb13` | Games, team sets, departures and caps | `EachSeatReportsGamesBeforeTheTeamFinalAndSeesItsTeamScore`; `HostBeginsOnlyAfterTheFourSeatGameIsPlugged`; `ALeaverLosesItsWholeTeamIncludingWhenItIsOurPartner`; `OrcaStatus.CoSeatsCapsAreAppendOnlyAndFitLegacyReaders` |
| This commit | Grouped unique probes | `OrcaRoomPingRange.BurstDeliveredUniqueProbesAreAcknowledgedUpToSix`; `FloodAndRepeatedNoncesAreBounded` |

The corrected original-engine run captured **9 tests, 9 failures, 0 errors**, in
`before-corrected.xml`. The initial new room fixture omitted its lifecycle field; it was
corrected before collecting this result. Original engine sources were checked identical to
`dd314fb40a`; the preserved baseline binary differs only in generated commit metadata from
the contract-first checkout. The corrected baseline build restored current sources in a
`finally` block. No other checkout or worktree was created.

## Memory and colours

The native result reader now reads the per-player team byte at record offset `+0x06`.
The retained four-machine records and the new fixture agree on that byte; stock and placement
fields remain per-player. The native winning colour expands to both teammates, including an
eliminated partner, then must match the room's assigned pair. Native colours and room team
labels are independent. Opposing native teams tied for first produce a draw.

Team battle and colours stay with the host. No guest team writes were added: a deterministic
frame/event applying this I/O metadata under rollback has not been demonstrated. Doubles
keeps the two-port singles rule/SetBlock path disabled. This is the allowed A2 boundary.

## Capability offers

Exact offers with co-seats enabled, pinned for RSBE01, PPLUS32 and PPLUS321:

Session, **24 capability words** after the `orca caps` prefix:

```text
orca caps join leave stats pause delay perf direct host chat yougame gfx music results locks queue2 pick-casual pick-ranked talk rumble record hostui coseats qdoubles rdoubles
```

Product playback, **23 capability words**:

```text
orca caps leave stats pause delay perf chat yougame gfx music results locks queue2 pick-casual pick-ranked talk rumble replay broadcast record hostui coseats qdoubles rdoubles
```

`rdoubles` is last after `qdoubles`; co-seats off removes only `coseats` (23/22 words).

## Gates

- Fresh `build-rdoubles`: Release, tests/NOGUI on; Qt, autoupdate and bundle postprocessing off;
  compiler launchers disabled. Mac `tests dolphin-nogui` build passed.
- Focused: **60 tests, 0 failures**, plus the contract/source-citation test passed.
- Full `*Orca*:*Broadcast*`: **1568 cases in 156 suites, 1545 passed,
  23 opt-in skips, 0 failures/errors**, 685.677 seconds.
- `keyCode baseline=f86645e20656efe5 current=f86645e20656efe5`. No key-function changes.
- RSBE01, PPLUS32 and PPLUS321: baseline and final exited 0; each comparison contains
  **601 frames, 0–600, 0 differences**. No hash exclusions or differences to explain.
- Every game process used `-v Null -C "Dolphin.DSP.Backend=No Audio Output"`, isolated worktree
  user directories, read-only SD configuration and the existing per-process network denial.
- Builds/suites used the prescribed heavy slot, label `orca:codex-rdoubles-0391`, shared flock,
  nice 10 and at most two build workers. Submission loads were below 20. No lock was broken.

Evidence is retained in ignored `build-rdoubles/evidence/`: baseline provenance/source check,
build logs, `before-corrected.xml`, `focused.xml`, `full.xml`, `full-summary.json`,
`keyCode.txt`, baseline/final hash summaries and six profile hashlogs. Tests used loopback
fixtures and cleared inherited live-test opt-ins.

No engine gates blocked. Rooms/site integration remains doubles-lead's counterpart task;
the contract explicitly describes its authoritative team agreement, set and rating policy.
