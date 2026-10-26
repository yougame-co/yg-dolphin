# prep-0391 verification

Engine worktree and branch remained `orca-ci-src`, `rdoubles-0391`. No version bump or push.

Initial base check:

```text
1c41c01f30 Acknowledge grouped queue probes
BASE-OK
```

## Failing-before evidence

All XML/log evidence is retained in ignored `build-rdoubles/prep-0391/`.

| Change | Regression (suite prefix omitted where repeated) | Before | After |
| --- | --- | --- | --- |
| `4e42a2216e` arrivals | `OrcaRoomRankedDoubles.ArrivalsAreAdmittedUntilFourCanBegin`, `InvalidArrivalsRefuseOnceAndSeenSeatsKeepTheirTeam`, `PartialStartCannotFreezeAMatch` | 3 failures in `arrival-before-corrected.xml` | 11 ranked-doubles cases passed |
| `c7fc13d0a8` local-add | `LocalAddIsRefusedWithRankedReasonBeforeSending` (both page timings) | 1 failure | 13 related cases passed |
| `fd66b0ae5a` tickets | `OrcaRoomRuntime.RankedDoublesTicketsOmitLocalSeatsAtMintAndRefresh`, `OrcaRoomRankedDoubles.TicketSeatsAreAbsentForRankedRequestsAndKnownRooms` | 2 failures; 2 casual checks passed | all 4 passed |
| `7d6d437dd9` room notices | `ARoomNoticeWithoutARosterKeepsThePartialSeats`, `ASeatFirstSeenAtStartKeepsItsTeamForTheNextStart` | 2 failures | both passed |
| `e9080c0329` file reasons | `OrcaBootAudit.*Reason*`: open, missing, non-regular, known size, hash, changed | 6 failures | all 36 boot-audit cases passed |
| `edca2b41d4` legacy resources | `OrcaPPlus321.LegacyFilesAreByteIdenticalTo647d1aa6c7` | failed in `legacy-before.xml` after size metadata was added to INIs | resources restored byte-identical to base; included in focused/full rerun |
| `f688e39f87` revision reasons | `OrcaDisc.AliasRefusalNamesEachFailedCheck`, `RevisionRefusalKeepsTheProfileSentencePrefix` | both failed | 28 focused cases: 25 passed, 3 opt-in skips, 0 failures |
| Contract/evidence commit | `ResultContract.test_ranked_admission_and_ticket_contract`, existing source anchors | failed | See `contract-after.log` |

The corrected arrival red run rebuilt the original engine source from `1c41c01f30` with the
new tests, restoring current source in `finally`. The file-reason red run added only an inert
optional size field so the new size test could compile against the unchanged audit behavior.
The revision red run added only output/formatter signature scaffolding, with the alias's
original behavior and a prefix-only formatter. No implementation fix was present in those red runs.
Existing message assertions were updated to expect the suffix while pinning their old prefix.
The final contract commit also pins exact `ls` key absence, with a fixture presence flag;
casual requests must retain the key. These assertions run in the final full suite.

## Admission rule

Accept 1–4 valid unique single-seat participants, at most two per team, with each previously
seen slot keeping its team; require four distinct accounts and complete 2+2 only at begin/start.
Worker snapshot `2fbe56330` was read locally, without service requests or modifications.
Its `away` notices can omit participants, so those retain the roster. Teams first observed at
start are also remembered for a later start. Desktop rebuilds mpTicket from a fixed field list.

Optional `LauncherSize` / `SDImageSize` profile metadata enables the size-specific refusal.
The shipped legacy INIs have no size metadata, so those skip the size shortcut, as the brief
permits; their byte-for-byte resource pin is preserved. Hash verification still runs.

## Refusal examples

The unchanged prefix for the following suffixes is:

```text
Orca: /games/PPLUS321/sd.raw is missing or is not the SD card profile PPLUS321 names
```

- ` (open failed: Permission denied)` (Windows uses its OS message and numeric error).
- ` (missing)`.
- ` (not a regular file)`.
- ` (size differs: actual 1 bytes, expected 2097152000 bytes)`.
- ` (hash differs: actual 01234567, expected 5a9d851e)`.
- ` (changed while being verified)`.

An unchanged disc-profile prefix with its reason:

```text
This disc is RSBE01 revision 1; YG Dolphin's profile is for revision 2 (content[0] SHA-1=00000000; expected ab93ef28)
```

`AliasRefusalNamesEachFailedCheck` pins platform, game ID, revision (including absent), invalid
TMD, content counts 0/2, SHA-1, all three patch entries and an unreadable patch byte. Patch reasons
include index, partition, offset, actual byte (or unreadable) and expected byte. The reason is
captured from the original attempt and appended without retrying the alias.

## Final gates

- Mac Release build `tests dolphin-nogui`: exit 0, reusing `build-rdoubles`, two workers.
- Full `*Orca*:*Broadcast*`: **1585 cases**, 0 failures,
  0 errors, 23 opt-in skips; 685.302 seconds.
- Caps test passed for all three profiles: session **24 words**, product **23**, `rdoubles` last;
  co-seats off remains 23/22.
- `keyCode baseline=f86645e20656efe5 current=f86645e20656efe5`.
- RSBE01, PPLUS32 and PPLUS321: exit 0, **601 frames each (0–600), 0 differences**
  against the retained `dd314fb40a` baseline hashlogs and provenance in `build-rdoubles/evidence/`.
- Every game used `-v Null -C "Dolphin.DSP.Backend=No Audio Output"`, isolated user directories,
  read-only SD settings and the existing per-process network denial. Fixtures used loopback
  servers and a minimal environment without inherited live-test opt-ins.
- All builds/suites used the prescribed heavy-slot label `orca:codex-prep-0391`, shared
  `flock`, nice 10 and at most two workers. Long scheduler/lock waits were allowed to finish.

No gate blocked. No engine version bump, push, release, signing or service changes.
