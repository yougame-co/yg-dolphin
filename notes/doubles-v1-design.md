# Friends-room doubles v1

Base: `3bfe032623`, YG Dolphin 0.3.44, compatibility 30. Private local work.

## Contract and admission

Only an authoritative `room.queue = "private"` welcome with optional boolean
`room.doubles = true` enables the new path. Size remains the existing optional
`room.size` (2..4). The room service assigns ports 2..4 in arrival order; Dolphin
uses the authoritative slots and never independently renumbers another player.
Optional boolean `hello.doubles = true` advertises support. Absent fields retain
the existing one-friend policy; queue rooms cannot enable doubles. A legacy peer
can still play an ordinary two-player room without new fields or packet bytes.

The host gathers arrivals for two seconds from the first arrival, closing early
when its three friend slots are filled. Admission/offers happen only on the main
menu or character select (`MenuReads::OnTheMenus`) and outside single-player
modes. Arrivals during a fight remain waiting, with the existing hold notice,
until the next menu. An arrival during an offered batch waits for its completion.
Recovery of an already retained seat bypasses the menu restriction.

One capture, one replay id, one final names/controls roster and one live-from
frame serve the whole batch. Every member acknowledges the complete roster and
rebuilds before the host announces one common plug frame. That frame includes
the worst member's RTT allowance. The host suppresses Start while the batch is
pending, preventing a fight from starting during capture/rebuild. New members
remain unplugged until the whole batch is ready. No rotation is introduced.

## Inputs, mesh and recovery

The existing six per-pair direct links and per-pair TURN fallback are retained.
For multiple friends, input ranges are selected separately for each recipient's
ack/frame. One optional relay batch envelope carries the recipient packets in
one room message; direct packets use the existing encoding and target one pair.
This prevents a rebuilding peer from withholding inputs from seated peers.
History service rotates between rebuilding seats. Ordinary one-friend sessions
keep the existing broadcast, encoding, history order and timing decisions.
The optional host packet `sg = [from, until]` carries the Start gate alongside
the roster, so every peer applies it to the same simulated frames. The `bp`
envelope has at most three `[receivingSeat, ordinaryPacket]` entries. Each
coalesced history is capped at 128 frames to keep all three under 64 KB.

Away/back, input holds, room reconnect budgets and stall budgets remain per peer
as in 0.3.44. Buffered blackouts stall all four and resume the same session/code.
A retained peer needing a keyframe rebuilds through its existing recovery path;
recipient input ranges keep the other peers progressing after its unplug. A
host with continuous predictions captures the newest final replay checkpoint
using its saved RAM hash, rather than waiting for a fully settled live frame. A
real friend leave drops only that port. Stall holds select only the missing-input
seats; an expired friend's input hold drops that seat. Fatal session/desync policy
remains unchanged. Checksums are retained per peer and frame, so one peer's report
cannot replace another's. Permanent host departure retains the existing hosted
session-end behavior; host migration is not introduced.

Shared stored captures belong to the host until all batch members plug in (or
the room ends); joiners must not delete them after their individual replay.
Optional keyframe `shared = true` marks this ownership. Compatibility stays 30;
multi-peer envelopes are used only in opted-in capable rooms.

## Game rules and UX

Friends-room rules remain the game's casual With Friends rules, with no ranked
header, queue result or replay-stream changes. Names and each player's own
controls use the existing per-port values/version mechanism for all four ports.
P+ and vanilla Brawl expose team battle, stock/time and port/team colours in
their own four-port character select. The host chooses teams and casual rules
there; v1 does not write game-specific team offsets or force a ranked preset.
The release lead must verify both games' CSS, team colours and controls on real
Mac/PC instances. Room/site creation and its doubles toggle are another lead's
work; this contract alone does not create or publish a room.

## Gates

Tests exercise three-member batch readiness, atomic plug frames, per-recipient
fan-out, history fairness, late arrivals, one peer leaving, buffered blackout,
retained-seat resync and two-player packet/frame equivalence. The four-instance
harness is loopback/dev-ticket only here, with a dry self-test; real Mac/PC play
belongs to the release lead. Builds/suites/shadow checks use the shared lock,
nice 10 and at most two test workers. No external service, push or release.
