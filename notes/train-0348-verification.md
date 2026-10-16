# YG Dolphin 0.3.48 verification

Task `train-0348`, 2026-10-16, private local commits on the existing branch.

## Required base check

Before edits:

```text
15621acde9 Record train 0.3.48 configuration blocker
BASE-OK
```

`git merge-base --is-ancestor 694f5f9644 HEAD` succeeded. The initial worktree was
clean. The restored submodules resolved the earlier curl configuration blocker;
its historical record commit remains in ancestry. No merges, rebases or pushes.

## A: replay fixtures — PASS

`OrcaReplayStream.HeaderBytesMatchRoomsGoldenFixture` compares the complete real
`PackReplayStreamHeader(StreamInfo())` output with exact hex for both seat-label
variants. `StreamInfo()` matches the supplied fields; no correction was needed.
The comment points to `rooms/test/fixtures/yg-dolphin-stream-header.json` and its
`about` refresh steps. Container version 1 and JSON frame-layout v=2 are distinct.

Plain, 183 bytes (JSON 171 bytes):

```text
5947525301000000ab0000007b226275696c64223a2230313233343536373839616263646566222c22636f6d706174223a226f726361323a74657374222c2266697273745f6672616d65223a3530302c2267616d65223a22525342453031222c226f726967696e5f68617368223a2231323334353637383961626364656630222c22726f6f6d223a22616263313233222c22736561745f6c6162656c73223a66616c73652c227365676d656e74223a322c2276223a327d
```

Seat labels, 182 bytes (JSON 170 bytes):

```text
5947525301000000aa0000007b226275696c64223a2230313233343536373839616263646566222c22636f6d706174223a226f726361323a74657374222c2266697273745f6672616d65223a3530302c2267616d65223a22525342453031222c226f726967696e5f68617368223a2231323334353637383961626364656630222c22726f6f6d223a22616263313233222c22736561745f6c6162656c73223a747275652c227365676d656e74223a322c2276223a327d
```

The real `Stream(Replay(30, 0), 10)` sample is **1,516 bytes**, with three chunks;
no shorter replay was needed. Its header first_frame is 0, as the helper takes
that field from the replay, while the header goldens use 500. SHA-256:
`36d2a4e821fd27739e20fcd4726b42be65d1c3420b2e7a47bc47ea79f2222c7e`.
The uninterrupted stream hex, generation steps and metadata are in
[replay fixture](train-0348-replay-fixture.md). The C++ test exports the actual
sample as the `chunked_fixture_hex` XML property.
Evidence: [1/1 golden test](train-0348-golden.log), [XML](train-0348-golden.xml).

## B: friendlies — PASS for every requested case

The runtime fix lets a connected player return their held token to their own
human panel in a friends room with at least three plugged ports. The old gate
masked that A, so unpicking alone kept the panel active and blocked rotation.
The regression failed before and passed after. No bench state, rotation message
or replay encoder change was added. Ordinary two-player and ranked input gates
are preserved and tested.

This emulated-input change requires the **Brawl and Project+ game revisions
0 -> 1**, key suffix `ux=31.1`: an older gate would process that A differently.
Engine compatibility stays **31**; results-only and legacy-launcher revisions
stay unchanged. Historical compatibility serialization goldens remain pinned.
The selected UI/key regression run passed **25/25**; the fixture runner has
**3/3** regressions for role ordering, blackout setup parity and bounded writes.
See [friendlies verification](friendlies-0348-verification.md) for root causes,
before/after tests, earlier failed/invalid attempts, controls and reproduction.

All final runs have fixture PASS lines, fight/CSS evidence, matching confirmed
hashes and **0 desyncs**. Evidence directories are under `notes/friendlies-0348-runs/`.

| Case | Verdict and proof | Confirmed frame | Evidence directory |
| --- | --- | --- | --- |
| 1: three peers, two fight, one sits out | PASS, repeated in rotation fight 1; humans 1/2, port 3 idle; back to CSS | 7740 across run | `case-2-attempt-7/` |
| 2: rotate | PASS, fight 2 humans 1/3, port 2 empty; back to CSS | 7740 | `case-2-attempt-7/` |
| 3a: four peers, two sit out | PASS, humans 1/2, ports 3/4 idle; back to CSS | 9000 | `case-3-attempt-3/` |
| 3b: four play 2v2 | PASS, teams 1+3 / 2+4; joint winners 1/3 each keep a stock; all four agree; back to CSS | 9000 | `case-3-attempt-3/teams-evidence.json` |
| 4a: sitting-out peer leaves | PASS, only port 3 drops; fighters continue to CSS | 6000 | `case-4a-attempt-1/` |
| 4b: sitting-out peer away/back | PASS, same peer/seat retained within budget; fight back to CSS | 5940 | `case-4b-attempt-1/` |
| 4c: sitting-out peer blackout | PASS, six-second blackout at 50–56 s; same seat; one counted stall; back to CSS | 5640 | `case-4c-attempt-4/` |
| 5: third arrives mid-fight | PASS, holds until CSS at 4044, then plugs at 4455; fighters finish normally | 5940 | `case-5-attempt-1/` |
| 6: third arrives during recovery | PASS, retained friend keeps seat 2 through lost relay/direct traffic; newcomer plugs at 4466 after CSS 4045 | 5580 | `case-6-attempt-1/` |
| 7: fifth arrival | PASS, fifth ticket HTTP 409; original four finish fight and CSS | 6000 | `case-7-attempt-1/` |
| 8: ordinary room third arrival | PASS, only third gets room_full and never plugs; original pair finish fight and CSS | 5940 | `case-8-attempt-1/` |

Doubles rooms are explicitly opted in through the existing room flag. Ordinary
rooms retain the prior one-friend policy. No site/default toggle change belongs
to this worktree. The authorized Brawl WBFS was read in place, never copied;
no sparsebundle or disk image was attached or opened.

## C: 0.3.48 version and player notes

- `Source/Core/CMakeLists.txt` defaults ORCA_VERSION to **0.3.48**. The existing
  build cache was explicitly reconfigured with `-DORCA_VERSION=0.3.48`.
- Both package scripts now use 0.3.48 in every old 0.3.47 fallback (Linux version,
  macOS version and plist version). A scan of those defaults finds no 0.3.47.
- `ORCA.md` has 0.3.48 above 0.3.47: up to four in doubles friends rooms, sitting
  out/rotating and the native token action, clear newer-pack update message,
  and the known limit that a sitting-out friend's connection can stall a fight.
- E2 is an opt-in pack capability; this train does not ship/prove a new playable
  results-only pack. No standalone ranked-mod availability promise was added
  to the player notes.

Version configuration and both targets succeeded:
[configure](train-0348-version-configure.log), [build](train-0348-final-build.log).
No packaging, release, signing or deployment was run.

## Final gates

| Gate | Final result | Evidence |
| --- | --- | --- |
| Release tests + NoGUI targets, version 0.3.48 | PASS | [build](train-0348-final-build.log) |
| Full binary with loopback room/keyframe fixtures and authorized disc | **2,157 passed, 21 skipped, 0 failures**, 2,178 selected / 172 suites | [full loopback log](train-0348-full-loopback-tests.log) |
| Full `--gtest_filter=Orca*` with the same fixtures/disc | **1,085 passed, 21 skipped, 0 failures**, 1,106 selected / 117 suites | [Orca log](train-0348-orca-tests.log) |
| `doubles-rooms.py --doubles --self-test` | PASS: four clients, fifth refusal, six pairs, capture/recovery/leave/expiry checks | [self-test](train-0348-doubles-selftest.log) |
| `compat-key-template.py selftest` | **78/78 vectors PASS** | [self-test](train-0348-compat-template-selftest.log) |
| Python runner regressions with warnings as errors | **3/3 PASS** | [regressions](train-0348-fixture-regressions.log) |
| Touched C++ with `-Werror` | **4/4 actual object compiles PASS**; touched headers checked by includes | [compiler log](train-0348-werror-final.log), [reproducer](train-0348-werror.py) |
| Friendlies gameplay cases 1–8, all subcases | PASS; every final run 0 desync | Matrix above and [detailed proof](friendlies-0348-verification.md) |

The default all-suite run already completed **2,096 passed / 82 optional skips /
0 failures**, 2,178 selected across 172 suites:
[default full test log](train-0348-full-tests.log). The expanded run uses the
existing `Tools/orca/room-fix-tests.py` loopback room/keyframe fixtures and the
authorized read-only disc via [test launcher](train-0348-tests.py), with a
minimal environment and no live-test opt-ins. The 21 remaining skips are:
14 live-service opt-ins; 1 live bridge/room check; 2 unavailable Rev 1-disc checks;
2 bridge-latency checks; 1 design-only export; and 1 Rev 2 check whose guard also
requires the unit binary's default macOS Sys resource layout. The supplied Rev 2
disc did enable the read-patch and font tests, and all live game proofs used it.
These guarded skips are never claimed as passes. No real external service or
credential was accessed. Required gates have 0 failures; no required work remains
blocked.

## Build and machine details

One build tree: `build-orca-tests`, Ninja, Release, arm64, macOS target 11.0,
tests and NoGUI ON; Qt, bundle and signing OFF. The cached train macOS options
also disable framedump encoding, LLVM, analytics, autoupdate, Discord, achievements,
mGBA, UPnP, Vulkan, CLI tool and sanitizers; bundled libraries, system iconv,
ccache and compile_commands are enabled. Configuration succeeded after the
lead restored submodules: [initial configure](train-0348-configure.log).

All builds/tests/fixtures used `flock /Users/luis/Code/brawl-dolphin/build.lock
nice -n 10`, at most six build jobs and one test worker; no heavy-slot call.
Ccache used `CCACHE_DIR=/Volumes/YGWorktrees/ClaudeWorktrees/orca-ccache` and
`CCACHE_REMOTE_STORAGE=file:/Users/luis/Code/brawl-dolphin/ccache|read-only`.
Preflight load was below 18; fixture starts additionally reserve headroom and
recheck after locking. The one earlier run stopped for load 18.35 is explicitly
not a gameplay verdict. One fixture at a time, at most four headless instances,
isolated temporary users, Null video and muted audio. Cleanup targets only
exact children started by the fixture. No installed app, personal account/profile,
controller, adapter, macOS privacy/security setting or other worktree was changed.
No pushes, PRs, production operations, external requests, signing or mailbox tools.
