# train-0346 verification

Base check before edits:

```
9849db6a48 Align controller fixture values
BASE-OK
```

Branch: `train-0346`. Worktree: `orca-rel-0344`. All changes and commits are local.

## Player text and replay verification

Live `NameTagsFrame` and `NativeText::Frame` use the synced real names again. The ZEKE and SANDB
expectations are restored. The live join archive also retains these names, because a joining peer
must reproduce the host's full RAM hash. `Writer::Pump` and its reopened-room backlog apply
`RecordingPorts` to their copies: persisted frame metadata holds only P1–P4, including after repacking.

`ReplayText` discounts only bytes actually changed by the name-text writers: the UTF-16 tag text
(including its cleared terminator), native text's changed message bytes (including shifted trailing
codes), and its length field. It hashes their pre-text values without modifying guest RAM. Bytes
outside those writes remain covered, including controls, tag indices, setup codes, buffer capacity,
data pointers, and unused buffer space. Later game writes that replace text bytes are covered.
Local text checkpoints follow rollback, origin restores and replay seeks. The recording's checksum
map is separate from `Session::LocalChecksums`; `RamChecksum`, snapshot hashes, COW hashes and live
join target verification keep full MEM1/MEM2 coverage.

Recordings also retain bounded, name-free tag assignment events (port, slot, created, apply controls).
Otherwise an existing saved name, duplicate name or unshowable username could change non-text slot
selection during anonymized playback. Their codec uses ORP3 / stream version 2; the Python stream
reader is updated. Earlier layouts are rejected by their version markers.

The headless replay-player regression records 121 frames through the real Writer and runs the real
Player twice: seat labels, then supplied view names. Each run checks two hashes with zero mismatches,
and the hook sees P1 / Viewer Name respectively. The name-tag and native-text tests additionally
exercise their real writers for 60 frames, verify real names and zero replay mismatches, and check
that adjacent non-text changes still affect verification. No game disc is used by these unit tests.

## Session and follow-ups

The host retains the earlier gate beginning until every in-play peer acknowledges past its end.
Only changed, unconfirmed inputs request a rollback. The receiver retains only finite previous gates.
The mesh regression drops 20 directed host packets over successive batches, then presses Start and
compares all surviving players' final frame hashes and controller masks. A separate receiver test
checks a moved gate after an open gate. Both pass.

The session retry loop explicitly counts down only when the in-play seat count did not drop; each
extra retry therefore requires a smaller roster. Tournament `MatchBegan` is intended: ticketed
private tournament matches already use `ReportQueue()`'s casual reporting/result lifecycle.

## Compatibility

Keep UX compatibility 31. Against 0.3.45 (`5f411d64be`), live name and native text byte values are
restored. The selected-controls change (`d3ff7951a8`) still omits the old writes to a worn default
tag's rumble (+0x0C) and layout (+0x14..+0x40), including C-stick (+0x1C), preserving the player's
selection instead of replacing it with their account profile. These are live RAM differences and
require isolation from compatibility 30. Doubles' batch Start masking also differs. Replay-only
accounting and name-free metadata add no further live RAM differences.

## Gates

All heavy commands use the shared build lock and nice 10; load was below 20 throughout. Only
`build-orca-tests` was built. Tests run serially; shadow checking used at most two workers.

- Required `cmake --build build-orca-tests --target tests -j4`: passed.
- All tests: 2,158 run; 2,076 passed; 82 fixture/opt-in skips; zero failed.
- Room/keyframe loopback fixture suite: 2,158 run; 2,135 passed; 23 opt-in skips; zero failed (591.674 s).
- `dropin-rooms.py --doubles --self-test --turn-binary build-orca-tests/Binaries/Tests/orca-turn-fixture`: passed (four clients, six pairs, recovery, leave, expiry, plug/hash checks).
- Clang `-Wshadow-all -Werror=shadow-all`: 13 affected translation units; zero failures/warnings.
- `git diff --check`: clean.

No external services, downloads, pushes, releases or production operations. Nothing blocked.
