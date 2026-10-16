# YG Dolphin 0.3.48 verification

Task `train-0348`, resumed 2026-10-16 on the existing branch and build directory.

Base check before any edits:

```text
15621acde9 Record train 0.3.48 configuration blocker
BASE-OK
```

The branch contains `694f5f9644`. Initial worktree was clean. The restored submodules
resolved the previous curl configuration blocker. One Ninja Release arm64 build,
`build-orca-tests`, has tests and NoGUI enabled, Qt/bundle/signing disabled and the
train's macOS flags. Configure succeeded; `tests` and `dolphin-nogui` built successfully.
All build/test commands used the shared flock, nice 10, six build jobs at most,
and a preflight 1-minute load below 18. Requested ccache paths were used.
Evidence: [configure log](train-0348-configure.log), [build completion](train-0348-build-resume.log).

## A: Replay fixtures — PASS

Added `OrcaReplayStream.HeaderBytesMatchRoomsGoldenFixture`, asserting the complete
hex for `seat_labels: false` and `true`, with the rooms fixture refresh comment.
The real output matches the supplied goldens exactly (183/182 bytes); both preambles
are `5947525301000000ab000000` / `5947525301000000aa000000`.
Complete golden hex, real 1,516-byte chunked stream and SHA-256 are recorded in
[replay fixture](train-0348-replay-fixture.md). The test's XML property exports the
real helper bytes for future refreshes.

- Golden test: **1/1 PASS**, both variants checked: [log](train-0348-golden.log).
- Touched C++ `-Werror`: **PASS**: [log](train-0348-werror.log).

## B and C — pending

Friendlies cases 2–8, any resulting fixes, the version bump and player notes remain.
Case 1's prior PASS is supplied by Luis; it is not a new run here.
Full test binary, full Orca filter, doubles self-test and compat-key selftest are pending.
The authorized disc exists at the supplied APFS path and will be read in place.
No pushes, production operations, external requests, signing or mailbox tools.
