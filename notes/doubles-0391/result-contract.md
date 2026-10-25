# Ranked doubles result contract (rdoubles-0391)

Engine-only handoff for doubles-lead. No version bump. This document is committed before
implementation; engine citations identify the paths being extended, with final anchors to be
refreshed in the implementation commits. Service citations are from YouGame's local
`origin/claude/v2`, read on 2026-10-25; no service changes are part of this branch.

## Admission and capabilities

The page obtains a ranked doubles match in mode `orca-<p>-2v2-ranked` (lowercase profile ID),
then sends the ordinary `host <code>` / `join <code>` command. The initial engine ticket still
names its profile mode, room code, lobby compatibility and `ls:1`; the page's matched-room
registration must retain the ranked doubles mode when resolving that code. The engine does
not derive matchmaking mode from the welcome or append a suffix to a friends ticket.

Welcome must contain `room:{queue:"ranked",doubles:true,size:4}` and four participants:
distinct nonempty `id` and `connectionId`, distinct slots 0–3, `localIndex:0` (or absent),
and integer `team:0|1`, exactly two seats on each team. Ranked doubles is four accounts,
one seat per computer. Invalid/missing assignments or wrong size refuse the room once with
`Room: ranked doubles requires four single-seat players and two complete teams`,
code `room_mismatch`. A changed/invalid team assignment before a match also refuses it;
an active match retains its original seat/team identities for results and departures.
Casual/friends doubles retains its existing admission and reporting policy.

`rdoubles` advertises this result contract, appended LAST immediately after `qdoubles`,
including when co-seats are disabled. The 24-word reader limit counts capability words
after `orca caps`: session 24, product playback 23 (23/22 with co-seats off).
The caps regression pins the exact offers for RSBE01, PPLUS32 and PPLUS321 in both modes.
This is an offer to the page, not a new menu command or a page capability requirement.

Engine paths: `Source/Core/Core/Orca/Session/YouGameRoom.cpp:5320` (welcome policy),
`:4802` (participant team parsing), `:3712` (host begins the room match),
`:6657` (PlayQueue keeps the two-port rules path off for doubles);
`Source/Core/Core/Orca/Status.cpp:220` (append-only offer).
The existing room handoff is documented in `notes/doubles-0386/room-contract.md:11`.

## Per-game and set reports

Each of the four seats sends `game-start` with `id:"g<first confirmed fight frame>"`,
then one `game-report` for that game, before its `finish` (the room calls this `$final`).
The host sends `begin` only when all four seats are available in the game.
Reports are emitted only from confirmed frames; rollback replaces predicted readings and
never produces a second report for a confirmed game.

A won game is:

```json
{"t":"game-report","matchId":"M","id":"gF","ranking":["winner1","winner2","loser1","loser2"],"detail":{"f":123,"st":2,"c":[0,3,2,1],"s":[1,0,0,0],"to":0,"tw":0}}
```

The ranking contains **all four participant IDs**, the WINNING TEAM's two players first,
each team internally ordered by seat. `detail.tw` is the winning room team, integer 0 or 1.
It is not a colour, player ID, port or stock total. A successful set final uses the same
team-first ranking and `detail:{"tw":0|1}`. First to two agreed wins ends the set;
draws and voids do not award a win. The existing set game limit ends an undecided set void.
All seats on the same team see the same win/loss and team score, including seats 3 and 4.

Engine paths: `Source/Core/Core/Orca/Games/Brawl/Results.cpp:151` (result reader),
`:186` (Judge), `:329` (confirmed tracker),
`Source/Core/Core/Orca/Engine/MatchTypes.cpp:10` (detail JSON),
`Source/Core/Core/Rollback/OnlineMatch.cpp:3761` (confirmed results to room reports),
`Source/Core/Core/Orca/Session/YouGameRoom.cpp:3742` (game serializer),
`:3825` (final serializer), `:4054` (agreed game tally).

Rooms currently accepts full rankings (`rooms/src/game-lobby.ts:866`), four-integer detail
arrays (`:877`), and forwards team fields (`rooms/src/room-rules.ts:316`,
`rooms/src/index.ts:2000`). **Required rooms work for doubles-lead:** any two reports that
disagree on `tw` must void that game, even if their rankings agree. Current agreement at
`game-lobby.ts:634` uses `outcomeKey`; detail selection at `:644` does not itself enforce
team agreement. Current Orca best-of-three adjudication is explicitly a two-player duel
(`:447`); extend it to two teams, wait for pending games before `$final`, and apply the
leaver rule to the whole team. The engine must not substitute for authoritative rooms
adjudication or individual/pair rating policy.

## Departures and voids

- A leaver's team loses the set once a fight has begun; a leave before any game starts is
  void/unplayed. Survivors report the other team's full ranking and `detail.tw`, while the
  room records who actually left. A departing engine sends its queued game reports before
  `leave`; it does not fabricate an unfinished game's result. Rooms remains authoritative.
- A disconnect retains the existing reconnect allowance; it is not an immediate game win.
  Expiry follows the leaver rule. Rooms' existing reason is
  `The opponent left the match and did not reconnect`; a stall uses
  `The opponent stopped playing` (`rooms/src/game-lobby.ts:737`).
- A desync sends `{"t":"finish","matchId":"M","void":true}`; the local reason is
  `desync`. A session fault uses local reason `fault`. Neither supplies a winning team.
- A game void sends `game-report` with its normal game ID and `void:true`; local reasons
  include `nocontest`, `ports`, `teams`, and `rules-<rule>`. No `detail.tw` on a void/draw.
  A draw uses `draw:true` and per-seat detail. The undecided-set limit uses `limit`.
  A conflicting room game verdict is displayed as `mismatch`; an unrecorded final as
  `unrecorded`. These short reasons are engine diagnostics, not invented Worker reason fields.
  Rooms' existing disagreement reason is
  `The match was cancelled, incomplete, or players reported different results` (`:657`).

Engine paths: `Source/Core/Core/Orca/Session/YouGameRoom.cpp:3089` (flush before goodbye),
`:3904` (desync), `:3911` (fault), `:4783` (departure notice), `:4006` (verdict reasons).
Rooms/site applies each player's doubles rating plus each pair's rating. Premade losing
partners both lose rating; a solo queue non-leaver keeps rating and match count. The engine
does not claim `premade`, `left` or ratings: rooms derives these from matchmaking and departures
(`room-rules.ts:327`), and the site selects `record_team_match_report`
(`web/src/app/api/multiplayer/match-result/route.ts:94`). Pack/history eligibility remains a
site gate; engine support alone does not make an unapproved pack rated.

## Game memory and ownership

Per-seat: characters `c` and stocks `s`, always four entries in port/seat order 0–3, even
though ranking is team ordered. Frame `f`, stage `st`, timeout `to` describe the game.
Per-team: room assignment, winning team `tw`, set wins and final outcome.
The singles detail format and ranked SetBlock remain unchanged for singles. Doubles uses
the native results screen, not the two-port ranked SetBlock or its stage/character locks.

Existing read-only evidence: `Source/Core/Core/Orca/Games/Brawl/Results.h:28` documents
`g_GameGlobal` +0x18 result info, per-port +0x24 + port*0x2AC, +0x0A stocks,
+0x0E place (0 winner), and decision +0x1378. `Results.cpp:166` already reads all four ports.
`notes/friendlies-0348-runs/case-3-attempt-3/teams-evidence.json:1` records four agreeing
machines with room teams [0,1,0,1], stocks [1,0,1,0], places [0,2,0,3]: the game's two
joint winners. Brawl rev 2 and P+ use the same executable/result layout
(`Results.cpp:484`, `ResultsVerified`). Winning result ports are mapped to the room's teams;
inconsistent winner/team data is void `teams`, never inferred from stocks alone.

Team battle and colours are **left to the host**. No guest team-memory writes are added:
there is no demonstrated rollback-safe frame/event carrying this room metadata into every
machine's guest state. This preserves A2's boundary from 0.3.88. The host must select the
room's teams before play; results must identify the winning team unambiguously.
