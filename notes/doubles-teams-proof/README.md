# Room doubles team pinning

Base: `c35990d105 YG Dolphin 0.3.96`; `BASE-OK` (merge-base ancestor check).
Worktree/branch: `orca-doubles-teams`. Local only, no release/version change or publication.

## Native memory discovery

[Address evidence](address-evidence.md) was collected before implementing native writes.
`discovery.py` runs an offline four-port harness: setting `ORCA_TEST_DEV_GAME` would instead
enable the online session path and suppress that harness's synthetic ports. The initial run
with that knob activated only port 1 and was stopped; no address claim uses that run.

```sh
export TMPDIR=/Volumes/YGWorktrees/tmp/orca-doubles-teams
YG_MIN_DISK_FREE_GB=17 bash /Users/luis/Code/brawl-dolphin/notes/lead-records/yougame-v2-org/tools/heavy-slot.sh run --pool gpu --prio 1 --label orca-doubles-teams -- flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 python3 /Users/luis/Documents/brawl-dolphin/run-lock.py check -- python3 notes/doubles-teams-proof/discovery.py
```

All probe commands are reads. `discovery-cycle.py` additionally presses the native top-bar
button repeatedly to establish the Team battle flag's on/off behavior. Launchers and SDs
are local clone copies of the existing P+ 3.2.1b kit; the disc is the existing local WBFS.

## Implementation

Only a complete map of seats 0..3, values 0/1, two per team, enables the pin. The host captures
its mask only where the existing header-free boundary permits writes. Pairing changes use
the header generation to invalidate unused stale keyframes and are typed replay events.
They occupy a previously unused eight-byte slice of the existing match block, independently
of the singles header. Rollback, catch-up and playback use that guest state. The native
per-frame writes never consult the local room, input, time or name.

Character select uses four area team words; stage select uses the snapshot which constructs
the fight. Team battle is never enabled by Orca. The once-per-select log latch is guest state.
No singles rules, ready timer or stage strikes are enabled for doubles. The Brawl/P+ game
compatibility revision is 2, keeping engines with the old behavior out of the same session.

The replay codec adds an optional typed pairing presence bit. Frames without that event retain
their bytes, as verified by existing serialization goldens; the stream container stays v1.

## Build and focused checks

```sh
export TMPDIR=/Volumes/YGWorktrees/tmp/orca-doubles-teams
YG_MIN_DISK_FREE_GB=17 bash /Users/luis/Code/brawl-dolphin/notes/lead-records/yougame-v2-org/tools/heavy-slot.sh run --pool heavy --label orca-doubles-teams -- flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 bash -c 'cmake -S . -B build-doubles-teams -G Ninja -C notes/doubles-teams-proof/configure.cmake -DENABLE_TESTS=ON && ninja -C build-doubles-teams -j 8 && ninja -C build-doubles-teams -j 8 tests'
```

An initial test compilation needed a missing Replay.h include. It was corrected; the final
incremental engine and tests build passed (`build-pin-final.log`). Installed Homebrew FFmpeg
libraries produced deployment-target warnings (macOS 26 libraries / macOS 11 target).

The focused executable ran through the same heavy/flock/nice wrapper, with an explicit child
environment containing only PATH and the authorized TMPDIR, and these arguments:

```text
build-doubles-teams/Binaries/Tests/tests
--gtest_filter=OrcaOnlineRules.*:OrcaKeyframe.*:OrcaReplayStream.*:OrcaMod.UxKeyPartIsPerGame:OrcaMod.GoldenLegacyCompatibilityKey:OrcaResults.*
--gtest_output=xml:notes/doubles-teams-proof/focused.xml
```

124 passed, 1 opt-in HTTP fixture skipped, 0 failures. New tests cover complete/invalid maps,
Team battle off, both pairings, changed native colour correction, rollback restoration,
typed replay recording and replay repacking, event rejection, and stale-keyframe generation.

## Four-engine proofs

Run separately, waiting for each to finish:

```sh
bash notes/doubles-teams-proof/run-proof.sh pair12
python3 notes/doubles-teams-proof/verify-proof.py pair12
bash notes/doubles-teams-proof/run-proof.sh pair13
python3 notes/doubles-teams-proof/verify-proof.py pair13
bash notes/doubles-teams-proof/run-proof.sh no-teams
python3 notes/doubles-teams-proof/verify-proof.py no-teams
```

`run-proof.sh` contains the exact fixture command and all slot, run-lock, headless and mute
arguments. It starts an isolated loopback server and four engines, one proof at a time.
Raw logs, diagnostic checksums and binary reads remain in this directory's case folders.

Results (all four engines agree on each listed frame and report):

| Room seats | Native colours | Fight starts | Result frames | Reports |
| --- | --- | --- | --- | --- |
| 1+2 vs 3+4 | 0,0,1,1 | 2346, 5633 | 4379, 7657 | port 1 won, `tw:0`, twice |
| 1+3 vs 2+4 | 0,1,0,1 | 2346, 5584 | 4330, 7562 | port 1 won, `tw:0`, twice |
| No assigned teams | 1,0,1,0 | 2346, 5585 | 4331, 7563 | existing `void (teams)`, twice |

Every case passed 173 common confirmed checksums through frame 10680, zero desyncs, and two
fights returned to CSS. The two assigned fixtures each accepted a 2–0 team final, ranking the
correct winning pair first. Every engine logged the assigned pair once in each select (including
the third select after game 2). Teamless engines logged no pin, and every pairing-storage read
remained the original eight bytes `3a 20 00 01 48 00 07 bc`. Native Team battle was off at CSS
1400 and on at 1460 in every case; while off, all four native colour words remained zero.

[Machine-readable summary](proof-summary.json) retains the frame/report and local verdict data.
The independent verifier checks all four CSS, stage-select and result colour records, surviving
stocks, unchanged storage without a room pairing, and identical report contents and frames.

The first pair12 run used a relative scratch path, which nested the engines' probe directories.
Before fixture verification, its eight `probe-fight-N.bin` files were extracted unchanged from
the already saved full melee-init dumps, bytes `[0x98:0x208]`. Subsequent runs use an absolute
scratch directory and dump those fighter records directly. The first shell wrapper reported a
trailing parse error after its completed child returned 0 because its script was edited during
execution; the engine fixture, checksum validation and independent evidence verifier passed.
The saved runner passes `bash -n` and is left unchanged during subsequent runs.

The teamless runner's first preflight exposed macOS Bash's empty-array behavior under `set -u`;
its next attempt used an overlong room code and stopped before reaching four seats. Both fixture
issues were corrected locally (explicit valid `noteams4` room); no engine source changed.

## Limits

All engine proofs use P+ 3.2.1b / Brawl rev 2, headless Null. Visible select rendering and a
separate vanilla Brawl boot were not verified. Live join histories exercise the typed event;
rollback restoration and persisted replay repacking are unit-tested, without a separate
interactive saved-replay or forced-latency run. Native P+ modes 2 and 3 were read during
discovery; the four-engine proofs use regular Team battle (mode 1).

The unassigned-room reporting policy is unchanged: `FromGameResult` currently rejects an
empty doubles assignment as `teams`. That room's native colours/results remain host-selected;
the corrected reason log now explains its void reports. This task does not change that policy.
