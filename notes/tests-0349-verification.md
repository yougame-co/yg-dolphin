# tests-0349 verification

Worktree: `/Volumes/YGWorktrees/ClaudeWorktrees/orca-0348`, branch `train-0349`.
Base check before edits:

```text
b63badef57 Ride out a restarting room's 503s and put the loss class first
BASE-OK
```

Only tests, harnesses and these notes change. No engine behavior changes, version bump,
live queue run, external service requests, push or deployment.

## Changes

- `ServerClose503RefusalsKeepFriendsUntilTheRoomHoldExpires` polls throughout `ROOM_HOLD`,
  supplies a 503 after every attempted reconnect, polls again at the same timestamp,
  and checks every attempt count and scheduled retry. With midpoint jitter there are
  25 attempts: immediate, exponential backoff, the 10-second restart boundary, then
  one every four seconds. Failed upgrades do not consume the budget of six successful
  WebSocket connections in five minutes. The session remains alive until precisely
  close time + 90 seconds, ending with `network` and
  `Lost the connection to the room (hold-expired)`; the outcome logs once.
- `queue-e2e.mjs --record` adds `record` to each caps answer and immediately sends
  `record on`. It requires casual `match`, checks both pages observed the same room
  and match ids, and prints those ids. Casual ids come from `mpRoom`; `game-start`
  is ranked-only. The replay hand-off is server-to-server, so without a Worker log
  capture the run explicitly reports **RECORD HAND-OFF PENDING** with the required
  event to check. `--rooms-log` requires a successful delivery event for each played
  room, accepting plain event JSONL or Worker log envelopes. Missing/failed evidence
  fails the run. `--self-test` is dry and needs no binary, disc, account or network.
- `doubles-rooms.py --role-binary ROLE=PATH` selects binaries per process.
  `--version-refusals` carries each ticket's lobby and `ob` through the dev upgrade
  and returns the Worker's 403 JSON bodies for `host_older`, `host_newer` and
  `lobby_mismatch`. The real rule needs a different compatibility key to attribute
  a mismatch to versions; a version alone does not make matching keys incompatible.
  `--role-version ROLE=X.Y.Z` supplies the page's `ob`, which a direct dev-ticket
  binary does not send. `--owner-relaunch` permits the owner on another lobby to
  restart an empty room (including its own away seat) under the same code and logs
  `orca_room_started_over` with `why: owner_relaunch`; another live/held member
  prevents it. `--expect-version-refusal ROLE=CODE` requires the exact propagated
  `orca error room_mismatch <sentence>` and verifies the refused peer never seated.

Worker contracts were read from the local YouGame `origin/claude/v2` versions of
`rooms/src/index.ts`, `rooms/src/room-rules.ts`, and `rooms/src/packed-replay.ts`.
Delivery is `POST /api/replays/orca`, logged as `orca_replay_delivery` with `code`,
`status` and `ok`; it has no match id. A successful event proves the site accepted
the hand-off, not that the saved replay was subsequently played back.

## Local gates and evidence

All builds and test commands use the shared lock and `nice -n 10`. Builds use two
jobs, the test binary runs serially, and load is checked before starting. Raw local
evidence and the extracted app stay in the ignored `notes/tests-0349-runs/` directory.

```sh
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 \
  cmake --build build-orca-tests --target tests dolphin-nogui --parallel 2
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 \
  python3 Tools/orca/room-fix-tests.py build-orca-tests/Binaries/Tests/tests
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 \
  python3 Tools/orca/doubles-rooms.py --self-test
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 \
  node --check Tools/orca/queue-e2e.mjs
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 \
  node Tools/orca/queue-e2e.mjs --self-test
```

The room-fixture wrapper runs the full binary in a minimal environment with only
local fixture URLs, no live opt-ins, and all six required SDL controller switches.

Results (all exit 0):

| Gate | Result | Local evidence |
| --- | --- | --- |
| Build `tests` and `dolphin-nogui`, two jobs | Passed | `build.log` |
| Full test binary through loopback fixtures | 2,193 tests; 2,170 passed, 23 skipped, **0 failures**; 610.456 s | `full-tests.log` |
| Doubles-room self-tests | Passed existing mesh/recovery checks and all new version/relaunch checks | `doubles-self-test.log` |
| `node --check` | Passed | `queue-syntax.log` (empty on success) |
| Queue dry self-test | Passed caps/command order, match correlation, delivery evidence and failure controls | `queue-self-test.log` |

Evidence filenames above are under `notes/tests-0349-runs/`. The full binary ran
without a test filter. Its 23 skips are opt-in live/disc/probe checks; no live opt-ins
were enabled. The new 503 hold test passed. The fixture self-tests cover all three
exact 403 bodies, numeric version comparison, matching-key admission, owner relaunch
after both a leave and a held disconnect, and refusal while a friend still holds a seat.

## Two real binaries, local fixture only

The supplied 0.3.47 archive is extracted into
`notes/tests-0349-runs/staging-0347/`; the archive and extracted app are not modified.
This branch's binary is built in `build-orca-tests` and still identifies as 0.3.48
(the release version bump is outside this task). The disc is passed read-only by path.

```sh
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 \
  python3 Tools/orca/doubles-rooms.py --run --local --ordinary \
  --roles 0,1 --room versions349 --version-refusals \
  --binary "$PWD/build-orca-tests/Binaries/dolphin-emu-nogui" \
  --role-binary "0=$PWD/notes/tests-0349-runs/staging-0347/YG Dolphin.app/Contents/MacOS/Orca" \
  --role-version 0=0.3.47 --role-version 1=0.3.48 \
  --expect-version-refusal 1=host_older \
  --disc '/Volumes/YGWorktrees/OSCOO/ROMs/Wii/Super Smash Bros. Brawl (USA, Canada) (Rev 2).wbfs' \
  --keep-logs notes/tests-0349-runs/two-version
```

Expected: role 0 owns seat 1; role 1 emits `orca error room_mismatch Your friend is
on an older YG Dolphin. Ask them to press Play to update it and send you a new invite.`
Role 1 acquires no seat or controller port. Both headless processes use isolated
temporary users and the fixture's six SDL switches; cleanup targets only its children.

Result: **blocked**, command exit 1. The extracted 0.3.47 opener exited before it
owned a room seat; its captured stdout and stderr files are both empty. The harness
reported `RuntimeError: peer 0 exited before owning its room seat`. The fixture's
diagnostics show no active members or version events. Role 1 was never started, so
the real-binary `host_older` / `room_mismatch` outcome remains unverified.

Evidence: `notes/tests-0349-runs/two-version-run.log` and
`notes/tests-0349-runs/two-version/{peer-0.log,peer-0.stdout.log,diagnostics.json}`.
The startup cause is unknown from this evidence. No app changes or further launch
attempts were made. Validation stopped at this failure; all required gates above
had already passed. A runnable archived opener is needed to finish this check.

## Operator-only casual recording verification (not run here)

Use two signed-in **test** sessions and the intended staging listing. Supply the site,
listing, player ids and existing cookie file paths through these operator variables;
do not put cookie contents on the command line or in these notes. An authorized
operator should capture the rooms Worker's JSONL for this run to the local file
named below before starting. This task does not fetch logs or create sessions.

```sh
SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_HIDAPI_GAMECUBE=0 SDL_HIDAPI_LIBUSB=0 \
SDL_JOYSTICK_IOKIT=0 SDL_JOYSTICK_MFI=0 \
SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 \
flock /Users/luis/Code/brawl-dolphin/build.lock nice -n 10 \
  node Tools/orca/queue-e2e.mjs \
  "$PWD/build-orca-tests/Binaries/dolphin-emu-nogui" \
  '/Volumes/YGWorktrees/OSCOO/ROMs/Wii/Super Smash Bros. Brawl (USA, Canada) (Rev 2).wbfs' \
  --q2 --queue casual --scenario match --games 1 --record --keep \
  --site "$YG_TEST_SITE" --slug "$YG_TEST_SLUG" \
  --cookie-a "$YG_TEST_COOKIE_A_FILE" --cookie-b "$YG_TEST_COOKIE_B_FILE" \
  --id-a "$YG_TEST_PLAYER_A_ID" --id-b "$YG_TEST_PLAYER_B_ID" \
  --rooms-log "$PWD/notes/tests-0349-runs/rooms-recording.jsonl"
```

Expect both caps answers to include `record`, each immediately followed by `record on`,
matching casual verdicts, room/match ids from both pages, and
`PASS: replay hand-off: Worker POST /api/replays/orca succeeded ...`.
The script allows 35 seconds after both Orcas stop for captured delivery evidence.
Omit `--rooms-log` only for manual verification: the script prints the room/match ids
and leaves hand-off explicitly pending. For each printed room, find a Worker event
`ev: orca_replay_delivery`, matching `code`, `ok: true`, and a 2xx `status` after the
players leave. A missing event or `ok: false` does not pass recording verification.
