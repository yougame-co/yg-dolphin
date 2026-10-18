# recovery-0355 verification

Required initial base check (before edits):

```text
5097fe19d0 YG Dolphin 0.3.54
BASE-OK
```

Branch `recovery-0355`; local commits only. No version change, submodule initialization,
disc copy, dependency install, external service, push or deployment.

## Fix 1: match report outbox

Only opted-in public rooms retain the original encoded start/game/final reports.
Failed sends retry; unacknowledged game/final reports retry every two seconds, and
same-seat WELCOME replays the outbox without another gameplay callback. Retry uses
the retained bytes, without advancing report counters. Matching authoritative
receipts clear game/final entries; starts remain until the match ends. Existing
receipt deduplication still applies each result once. The outbox survives the
artificial-delay queue being discarded on reconnect. Legacy/friends sends retain
the existing policy. SendRaw now returns whether a write succeeded; delay queuing
is treated as pending delivery.

The fake transport can fail sends. `ReportsRetryOriginalBytesAndRejoinWithoutGameplay`
covers both queues, failed writes, successful writes with missing processing or
receipt, same-seat replay with no new callback, matching acknowledgements and
one local result. The existing ranked replay test now expects the required replay
of an unacknowledged game-start.

Build `tests` and `dolphin-nogui`: PASS. Focused recovery suite: 17/17 PASS.
`python3 Tools/orca/doubles-rooms.py --receipt-self-test`: PASS; loopback fake rooms
lose game/final receipts, retain them after match completion, and return the same
receipt for identical reports after rejoin. Logs: `build-recovery/recovery-0355-outbox-tests.log`
and `build-recovery/recovery-0355-receipt-fixture.log`.

## Web follow-up (review section 4)

This finding is web-only: terminal queue network status is mapped to rejoining
or generic network actions by web/src/lib/orca.ts. The web team should use queue
context and the actual terminal status sequence for both host and joiner, present
a terminal queue action, and preserve legacy network wording. No global engine
network-code or legacy wording change is made here.
