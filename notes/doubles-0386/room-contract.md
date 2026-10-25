# YG Dolphin 0.3.86 room contract

Engine-side handoff for doubles-lead (#15553). These are the current code paths, including
Part A of doubles-0386. `qdoubles` means “I play casual queue doubles rooms”; there is no
Doubles menu pick in this build. `ORCA_COSEATS=0` disables couch pairs; one seat per computer
still works. The append-only caps regression covers both session and product playback.

## Ticket request

`host <code>` and `join <code>` enter the named room. The command handler consumes the requests
at a frame boundary; `StartRoom` stores the code and joining role and disables asking the app
for another room. The initial ticket body is the same for both roles.

- `Source/Core/Core/Rollback/OnlineMatch.cpp:2998` — `JoinInPlay(match, *code);`
- `Source/Core/Core/Rollback/OnlineMatch.cpp:3006` — `HostInPlay(match, *code);`
- `Source/Core/Core/Orca/Session/Online.cpp:316` — `options.room_code = code;`
- `Source/Core/Core/Orca/Session/Online.cpp:320` — `options.ask_app_for_room = false;`

The desktop bridge request is `mpTicket`, with `room:<code>`, numeric `players:4`,
`mode:<profile mode>`, numeric `ls:1|2`, `ranked:false`, and `lobby`.
**Neither `queue` nor `doubles` is sent in this request.** `players:4` is room capacity, not
this computer's local seat count and not a request to select a doubles queue. The engine joins
the supplied code and reads its queue/doubles policy from welcome. The test site's direct
request carries the same room/players/mode/ls/lobby fields, without the bridge's `ranked` field.

- `Source/Core/Core/Orca/Session/Session.h:28` — `constexpr int MAX_SEATS = 4;`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:1830` — `request["players"] = picojson::value(static_cast<double>(MAX_SEATS));`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:1831` — `request["mode"] = picojson::value(options.mode);`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:1833` — `request["ranked"] = picojson::value(false);`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:1856` — `request["players"] = picojson::value(static_cast<double>(MAX_SEATS));`

With an active profile the mode is `orca-` plus its lowercased game ID:
`orca-rsbe01`, `orca-pplus32`, or `orca-pplus321`. It gets no queue/doubles suffix.
The initial lobby is `{version:1,minPlayers:2,maxLocalPlayers:2,compatibility:<engine key>}`.
`maxLocalPlayers:2` is a build constant even when co-seats are disabled or unagreed. `ls` is the
actual/requested local occupancy: 2 for an attached co-seat or pending local-add; otherwise 1.
An add requested before welcome counts as 2 only with co-seats enabled and agreed, outside
tournament tickets, while queue is unknown/private or casual doubles.

- `Source/Core/Core/Orca/Session/Online.cpp:276` — `options.mode = "orca-" + game;`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:1695` — `lobby["version"] = picojson::value(1.0);`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:1696` — `lobby["minPlayers"] = picojson::value(2.0);`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:1698` — `lobby["maxLocalPlayers"] = picojson::value(2.0);`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:1699` — `lobby["compatibility"] = picojson::value(options.compatibility);`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:1683` — `if (!my_co_seats.empty() || (local_add_pending && !inherited_co_cleanup))`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:1688` — `return allowed && page_add_before_welcome ? 2 : 1;`

Refreshes retain the original ticket/welcome lobby (a tournament reply without a lobby forces
`maxLocalPlayers:1`), reuse the mode and code, and recompute numeric `ls`.

- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:1882` — `ticket_lobby = original->second.get<picojson::object>();`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:1925` — `lobby = ticket_lobby;`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:1939` — `request["ls"] = picojson::value(static_cast<double>(TicketLocalSeats()));`

## Welcome

For casual queue doubles, send `t:"welcome"` with `room:{queue:"casual",doubles:true,size:4}`,
plus the usual `me`, `host`, `players`, `participants` and roster revision. `queue` retains
`casual` (or `ranked`; other strings normalize to `private`). `doubles` must be boolean true.
For casual, doubles activates only when parsed size is 4. Size accepts integers 2–4 and defaults
to 4 when absent/invalid; a four-seat room's explicit contract is `size:4`.
The page mirror retains the raw queue, size and doubles flag; gameplay uses `PlayQueue()`,
which maps doubles to `private` so singles queue rules do not run.

- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:5247` — `queue = q == "casual" || q == "ranked" ? q : "private";`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:5256` — `room_size = Int(room, "size", 2, MAX_SEATS).value_or(MAX_SEATS);`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:5260` — `doubles = wants_doubles && queue != "ranked" &&`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:3098` — `room["queue"] = picojson::value(queue);`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:6592` — `return m_impl->doubles ? "private" : m_impl->queue;`

Each participant names its `connectionId`, zero-based seat `slot` (0–3), and `localIndex`
(default 0; a co-seat uses 1), with numeric integer `team:0|1`. A complete valid map is stored
by seat and retained in the page mirror, including on lobby/start roster updates. If no team
fields occur, the map is empty. If any occur, all participants must have valid fields and
unique slots. One invalid/partial map latches teams off for that room, removes team fields
from its mirror, and emits one status line. A valid map emits
`Room: teams received; host chooses Team battle and team colours on the select` once.
The engine retains metadata; the host chooses Team battle/colours on the native select.
It does not automatically apply colours or reorder seats by team.

- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:4766` — `const auto team = Int(p, "team", 0, 1);`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:4775` — `teams_ignored |= invalid;`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:4778` — `seat_teams = any && valid && !teams_ignored ? std::move(parsed) : std::map<int, int>{};`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:4787` — `LocalStatus("Room: teams received; host chooses Team battle and team colours on the select");`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:4812` — `ReadTeams(room_participants);`

The expected service mode is the ticket's profile mode above. The engine sends `mode` on ticket
requests but does not read, compare, or require a welcome mode string; welcome parsing reads
room queue/lobby/recovery/stay/size/doubles. There is no separate doubles mode string here.
The original welcome `room.lobby`, if supplied on first welcome, replaces the retained lobby
for later ticket refreshes. The published page lobby instead derives its local-seat limit
from the current co-seat availability.

- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:5100` — `if (t == "welcome")`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:5251` — `ticket_lobby = original->second.get<picojson::object>();`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:3075` — `lobby["maxLocalPlayers"] = picojson::value(CoSeatsEnabledLocked() ? 2.0 : 1.0);`

## Ranked refusal

In 0.3.86, `room.queue:"ranked"` plus boolean `room.doubles:true` ends the room with
`Room: ranked doubles needs a newer YG Dolphin`, error code `room_mismatch`, once. This applies
regardless of size. Later incoming messages are ignored by the refusal latch.

- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:5259` — `refuse_ranked_doubles = queue == "ranked" && wants_doubles;`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:5271` — `const std::string line = "Room: ranked doubles needs a newer YG Dolphin";`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:5273` — `End(line, "room_mismatch");`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:5090` — `if (ranked_doubles_refused)`

## Results and reports

For admitted four-player doubles, 0.3.86 sends **no match `begin`, `game-start`, `game-report`,
or `finish`** in the supported casual doubles flow. Doubles always uses reporting queue
`private`: pending reports are discarded and `result` verdicts are ignored. Thus the casual
verdict path publishes no `orca result` for doubles.
This includes casual doubles and keeps it unrated. The existing
`OrcaRoom.CasualDoublesDoesNotSendSinglesResults` regression checks the outgoing message policy.

- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:3656` — `if (doubles)`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:3657` — `return "private";`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:3675` — `if (room_queue == "private" || !current_match.empty() || (room_queue == "ranked" && done))`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:3834` — `if (room_queue == "private")`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:3836` — `reports.pop_front();`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:4157` — `if (room_queue == "private" || match.empty() || !verdicts_seen.insert(match + "/$final").second)`

The underlying Brawl result reader reads four port records **per seat**, not team-ordered.
Its current judge accepts only humans on ports 1 and 2 with ports 3 and 4 absent; a four-player
results screen yields void reason `ports` (unless an earlier rules/no-contest check already
voided it). `GameResult` carries one winning port and two-element character/stock arrays.
Its detail JSON is `{"f":F,"st":S,"c":[c1,c2],"s":[s1,s2],"to":0|1}` in port order, with
ranked block metadata `k`, `lg`, `tb` when applicable. It is not a four-player/team verdict.
The frame-confirmed result is converted to an internal `GameReport` with
`winner_seat = winner_port`. The diagnostic log is
`Matchmaking: a game ended at frame F: void (ports)` for that four-player result (or its
earlier void reason); it is not a service report or an `orca result` line. No four-seat
ranking or team-ordered result array is serialized by this path.

- `Source/Core/Core/Orca/Games/Brawl/Results.cpp:166` — `for (u32 port = 0; port < 4; ++port)`
- `Source/Core/Core/Orca/Games/Brawl/Results.cpp:201` — `if (!p[0].present || !p[1].present || !p[0].human || !p[1].human || p[2].present || p[3].present)`
- `Source/Core/Core/Orca/Games/Brawl/Results.cpp:203` — `g.why = "ports";`
- `Source/Core/Core/Orca/Engine/MatchTypes.h:74` — `std::array<int, 2> characters{-1, -1};`
- `Source/Core/Core/Orca/Engine/MatchTypes.cpp:14` — `characters[0], characters[1], stocks[0], stocks[1], timeout ? 1 : 0);`
- `Source/Core/Core/Rollback/OnlineMatch.cpp:3787` — `report.winner_seat = r.winner_port;`
- `Source/Core/Core/Rollback/OnlineMatch.cpp:3802` — `NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: a game ended at frame {}: {}", r.frame,`

For comparison, singles casual reports use `{"t":"finish","matchId":M,"winner":P,"detail":D}`
(or `draw:true`/`void:true`), and ranked games use `game-report` with an ID. Winner IDs come
from seat/participant mapping, never from a team ID. Those serializers do not make doubles
rateable: the private reporting policy returns before reaching them. The existing `game-result`
handler is the ranked singles handler: its app line is
`orca result {"q":"ranked","k":"game","n":N,"out":O,"score":[mine,theirs],...}`,
with optional reason/detail, rather than a team verdict. Team result reporting and any 0.3.87
behavior are **not in this build**.

- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:3721` — `winner = ParticipantAt(r.winner_seat);`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:3747` — `fmt::format("{{\"t\":\"finish\",\"matchId\":{},{}}}", JsonString(match), outcome);`
- `Source/Core/Core/Orca/Session/YouGameRoom.cpp:4042` — `ResultLine(`
