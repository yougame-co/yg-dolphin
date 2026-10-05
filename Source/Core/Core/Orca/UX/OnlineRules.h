// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/MatchBlock.h"
#include "Core/Rollback/InputGate.h"

namespace Core
{
class CPUThreadGuard;
}

// The online rules for queue matches, Phase 1 (ORCA.md "Online rules"; YouGame
// docs/ORCA_ONLINE_UX.md): the match block's header, written at the alone boundary for a room the
// queue made, and the locks that follow from it at every boundary in both games.
//
// - The header: the host of a casual or ranked queue room writes mode and ruleset while its game
//   is its player's alone (no peer plugged in or on the way, nothing anyone replays), or while a
//   peer on the way still waits for a keyframe not yet made (SetHeaderFree). Anything
//   else (a friends room, solo play, a search still going, the queue room left) clears it at the
//   next such boundary, giving back the player's own rules saved when it was written. A ranked
//   room keeps it while the set is open, whoever is plugged in. A host makes no keyframe for a
//   queue room's opponent until its game carries that room's header (HeaderInPlace), and a joiner
//   checks it once its keyframe is in: anything but its room's own header (its queue's mode, its
//   ruleset, the room's coin and hash, its app's queue flags; never the host's own character
//   select's FLAG_SOLO) leaves the room ("mismatch").
// - The locks, while the header says casual or ranked: the ruleset's rules (stocks, time, items
//   off, handicap off, team attack on, ratio 1.0, pause off), the stage choice (Choose: casual's
//   two preferred-stage turns and ranked's strikes run on the stage select), the legal list (Brawl's Random Stage Switch without
//   Final Destination while someone is Ice Climbers; Project+'s 2024 Proposed list), and the input
//   gate's masks (no B on the character select, so no way back to the menus, and A only in the
//   character grid, so no player-type, name, team, rules or BACK button; no B on the stage
//   select; no Start in a fight, so no pause; Project+'s D-pad on the character select (its Code
//   Menu) and L, R and Z on the stage select (alternate stages, hazards, L+R+A)).
// - Ranked's character select timer: once one player has a character and the other hasn't, 60 s;
//   before the set's first fight, the one who still hasn't is a no-show and forfeits.
// Everything but the header is a pure function of emulated memory (the block included), written
// only where it differs, on first runs and re-runs alike: the same on both machines, idempotent.
// Friends rooms have no header, so nothing here touches their games, but for SessionMasks (no Z on
// the results screen in any session, below).
namespace Orca::UX
{
class GuestMemory;

namespace Rules
{
using MatchBlock::Mode;
using MatchBlock::Ruleset;

struct Header
{
  bool present = false;  // the magic and this version
  Mode mode = Mode::None;
  Ruleset ruleset = Ruleset::None;
  u8 flags = 0;  // MatchBlock::FLAG_SOLO, FLAG_QUEUE2 (Queue.h)
  u8 coin = 0;   // game 1's first striker (MatchBlock::COIN)
  u32 room = 0;  // the room code's hash (MatchBlock::ROOM)
  bool Locked() const { return present && mode != Mode::None && ruleset != Ruleset::None; }
  // The queue's own character select before a match (Queue.h): no room of the queue's yet.
  bool Solo() const { return Locked() && (flags & MatchBlock::FLAG_SOLO) != 0; }
  // The Slippi-style character select (Queue.h): its ready flags and timer, solo or in the room.
  bool Queue2() const { return Locked() && (flags & MatchBlock::FLAG_QUEUE2) != 0; }
  bool operator==(const Header&) const = default;
};
Header ReadHeader(const GuestMemory& memory);

// The ruleset the running profile plays: Brawl rev 2's, Project+'s, or none (no locks).
Ruleset ProfileRuleset();

// ---- The header, at an alone boundary ----
// Writes the header for `mode` (None: clears it, and gives back the rules saved when it was
// written), with game 1's first striker `coin` (0 or 1), the room code's hash `room` and the queue's
// `flags` (MatchBlock::FLAG_*). Entering a mode from none saves the player's own rules first; any
// new header (from none, or another mode, ruleset, coin, room or flags) starts every track's set
// state fresh. Returns the bytes changed (0: it already said so).
int WriteHeader(GuestMemory& memory, Mode mode, Ruleset ruleset, u8 coin = 0, u32 room = 0,
                u8 flags = 0);

// The header a game whose room calls for `mode` carries (none for Mode::None or no ruleset): with
// a queue room's `code`, game 1's first striker and the room's hash come from it, and `flags` are
// the queue's (FLAG_QUEUE2 or none); with FLAG_SOLO (the queue's own character select, no room's)
// there is no coin and no room. What the host writes (OnFrame) and what a joiner checks for.
Header HeaderFor(Mode mode, Ruleset ruleset, std::string_view code, u8 flags);
// Whether the header in memory, `in_memory`, is `wanted` (HeaderFor): the same header, every
// field; no header that locks when none is wanted.
bool HeaderIs(const Header& in_memory, const Header& wanted);

// ---- The locks, at every boundary ----
// `frame`: the frame hook's (the ranked ready timer counts from it). Returns the bytes changed (0
// without a header that locks).
int ApplyLocks(GuestMemory& memory, int frame);

// What the input gate masks for the frame about to run (reads only).
Rollback::InputGate::Masks GateMasks(const GuestMemory& memory);

// ---- Every session, with or without a header ----
// What the input gate masks in every session's game, friends and solo included (reads only): Z
// on the results screen (`scVsResult`), every port. Z there saves the fight's replay, in Brawl and
// in Project+ alike, and a session can't save one: its NAND has no save file and its SD card is
// absent (Brawl) or read-only (Project+). So Z only ever led to "There is no save file", then to
// the save's destination picker with both destinations crossed out, which A and Start never leave
// (only B does): players pressing A or Start after a game sat on the results screen for good, in
// both games (ORCA.md "The results screen's replay").
Rollback::InputGate::Masks SessionMasks(const GuestMemory& memory);

// ---- The character select's hands (both games; found under the harness, ORCA.md "The character
// select's buttons") ----
// Each player area (the character select's task +0x44 + 4 x port) has its hand at +0x1A8
// (muSelCharHand). The hand keeps what it points at, as of the frame that just ran: a kind of
// target (+0x80), the button when that is a button (+0xAC), and where it is (+0x90 x, +0x94 y, in
// screen units with y up; the character grid spans about y -5 to 16, the panels and their buttons
// are below it, the BACK, BRAWL tab, rules bar and Rules button above).
//
// A acts on whatever the hand points at once the frame's stick has moved it (probed: A pressed on
// the frame the hand arrives on a panel's player-type button switches that panel), so the gate
// passes A only to a hand in the grid or on its own token, and at least one frame's travel inside
// the grid's top and bottom edges (0.83 a frame in Brawl, 1.0 in Project+): no single frame can
// carry it onto a button with A down. Everything else on the screen is a button nobody needs in a
// queue match (the player-type buttons, which make a CPU of a player or of an empty panel; the
// name buttons; the panel's picture, a costume; the BRAWL tab, the team switch; the rules bar's
// arrows; Rules and BACK). Costumes still change with X and Y, and a player joins by moving the
// hand into the grid.
constexpr u32 CSS_HAND_NOTHING = 0;
constexpr u32 CSS_HAND_BUTTON = 1;  // the button at +0xAC: 0x1D a player-type button, 0x1C name, ...
constexpr u32 CSS_HAND_GRID = 2;    // the grid, the token down
constexpr u32 CSS_HAND_OWN_TOKEN = 3;  // A takes it up
constexpr u32 CSS_HAND_EXIT = 4;       // BACK (+0xAC 2) or Rules (4)
constexpr u32 CSS_HAND_TAKING = 6;     // the token on its way into the hand
constexpr u32 CSS_HAND_GRID_HOLDING = 7;  // the grid, the token in the hand (A puts it down)
constexpr u32 CSS_HAND_PLACING = 8;       // the token on its way down
constexpr u32 CSS_BUTTON_PLAYER_TYPE = 0x1D;
// A passes only strictly between these (one frame's travel inside the grid's edges). The top and
// Project+'s bottom: both games. Brawl's bottom row (RANDOM at its right end, Lucas, ...) reaches
// down to -4.07, and its grid goes on below it to -5.35 or lower, over nothing until the player's
// own panel picture (a button, 0x1B: from about -5.8): one frame's travel (0.83) from above -4.2
// stays in the grid, so Brawl's A reaches the whole of that row (Orca 0.3.19; with -3.0 the bottom
// quarter of every tile in it swallowed A, RANDOM's too: ORCA.md "Random and ranked's no skip").
constexpr float CSS_A_TOP = 15.0f;
constexpr float CSS_A_BOTTOM = -3.0f;
constexpr float CSS_A_BOTTOM_BRAWL = -4.2f;

struct CssHand
{
  bool valid = false;  // the hand could be read
  u32 target = CSS_HAND_NOTHING;
  u32 button = 0;
  float x = 0;
  float y = 0;
  bool operator==(const CssHand&) const = default;
};
// Port `port`'s (0-3) hand, on the character select; invalid anywhere else or before it is built.
CssHand ReadCssHand(const GuestMemory& memory, int port);
// Whether A may reach the game from this hand while the locks hold, `bottom` the game's lower edge
// (CSS_A_BOTTOM, Brawl's CSS_A_BOTTOM_BRAWL). An unreadable hand may not.
bool CssHandMayPressA(const CssHand& hand, float bottom = CSS_A_BOTTOM);

// ---- B on the character select: only to take the token back up (ORCA.md "The character
// select's buttons") ----
// Both games take a player's token back into the hand on a B pressed while it is down (a pick
// undone), and back out to the menus once B is held with the token in the hand (probed under the
// harness, ORCA.md). So while the locks hold B reaches the game only as the first frame of a press
// and only while that player's token is down: a B held on gets one frame of it, and a B with the
// token in the hand none, so the back out never starts. Whether the press is new comes from the
// queue's latch (Queue.h RAW: the raw buttons of the frame that just ran, written whenever a
// header locks), never from host state. The rules on top (a game-1 pick, a ready player, the
// character order's turns, Queue.h and CharOrder.h) mask B further.
struct CssToken
{
  bool valid = false;  // the player's area could be read
  bool human = false;
  int character = 0x28;  // under the hand while the token is in it, else the token's (0x28 none)
  bool in_hand = true;   // +0x1F8
  bool flying = false;   // +0x1F9: on its way up or down
  // On a character and still: B takes it up.
  bool Down() const { return valid && human && character != 0x28 && !in_hand && !flying; }
  bool operator==(const CssToken&) const = default;
};
// Port `port`'s (0-3) token, on the character select; invalid anywhere else.
CssToken ReadCssToken(const GuestMemory& memory, int port);
// Whether B may reach the game from a port whose token is `token`, its raw B held (`b_held`) on
// the frame that just ran or not. Pure.
constexpr bool CssBUnpicks(const CssToken& token, bool b_held)
{
  return token.Down() && !b_held;
}
// The same from memory: port `port`'s token, and its raw B from the latch (ports 1 and 2 only; 3
// and 4 never play a queue match).
bool CssBMayUnpick(const GuestMemory& memory, int port);

// Brawl's Random Stage Switch for the legal stages (Supernova 2025): the Brawl page's mask
// (record menu data +0x24) and the Melee page's (+0x20), without Final Destination when
// `no_final_destination`.
constexpr u32 BRAWL_LEGAL_BRAWL_PAGE = 0x04005007;  // BF, FD, Delfino, YI, Lylat, Smashville
constexpr u32 BRAWL_LEGAL_MELEE_PAGE = 0x00020000;  // Pokemon Stadium
constexpr u32 BRAWL_FINAL_DESTINATION_BIT = 0x00000002;

// The characters this module knows, in the match's init data (gmCharacterKind: the character
// select writes a port's pick there as it is made, and a Random pick resolved as it ends).
constexpr int CHARACTER_ICE_CLIMBERS = 0x10;
constexpr int CHARACTER_NONE = 0x3E;

// One port's pick, from the match's init data (gmGlobalModeMelee +0x98 + port x 0x5C).
struct PlayerPick
{
  bool human = false;  // a human's pick (type 0), not a CPU's or nobody's
  int character = -1;  // the character picked (gmCharacterKind), -1 none
};
std::array<PlayerPick, 4> ReadPicks(const GuestMemory& memory);

// The ranked character select's no-ready limit, in frames (60 s).
constexpr int CSS_READY_FRAMES = 60 * 60;

// ---- The session's side (Rollback/OnlineMatch.cpp, CPU thread) ----
// The header this machine's room calls for, set at every boundary before the frame hook (and again
// before a keyframe is made: a room's welcome can come in between): a queue room's while this
// machine hosts it (or waits in a ranked one for the set's verdict), `code` the room's code; the
// queue's own character select's (FLAG_SOLO, with the app's `queue2`) while its player waits there
// for a match; else none. `flags`: MatchBlock::FLAG_*. Returns whether that changed the header
// called for (WantedGeneration counts one more).
bool SetWanted(Mode mode, u8 flags = 0, std::string_view code = {});
Mode Wanted();
u8 WantedFlags();
// Counts every change of the header called for (SetWanted): a keyframe made under another count
// carries another header than the one called for now.
u64 WantedGeneration();
// Whether the game's memory carries the header called for (HeaderFor of SetWanted's), as the last
// first-run frame hook read it, with nothing changed since (SetWanted, MemoryReplaced). A host
// makes or offers no keyframe for a queue room until it does: the joiner would replay a game set
// up for another match (its own character select's header, another room's).
bool HeaderInPlace();
// The game's memory was replaced after this boundary's frame hook (the queue image): what the
// hook read of the header is stale until the next.
void MemoryReplaced();
// Whether this boundary's frame hook may write the header even though the frame isn't this
// player's alone (a friend's arrival or a prepare-join waits for a keyframe): set before the hook
// at every boundary (OnlineMatch's HeaderFreeAt). The header only: the rest of the UX keeps
// `alone`.
void SetHeaderFree(bool free);
// A joiner whose keyframe just loaded, into a room of the queue `mode` (none: a friends room) with
// code `code`, whose app plays the queue with `flags` (FLAG_QUEUE2 or none): the host's header is
// checked at the next frame hook. A friends room's game carries none; a queue room's carries that
// room's own (HeaderFor: never FLAG_SOLO, the room's coin and hash).
void ExpectHeader(Mode mode, std::string_view code = {}, u8 flags = 0);
// Whether a joiner of a room accepts the host's header `h` (ExpectHeader's check, pure).
bool HeaderFitsRoom(const Header& h, Mode mode, Ruleset ruleset, std::string_view code, u8 flags);
// "mismatch" once the check failed: the joiner leaves.
std::optional<std::string> TakeMismatch();
// The ranked no-show, once its frame is final: the port (0-based) that didn't ready in time.
// `confirmed`: frames through it are final (OnlineMatch's ConfirmResults).
std::optional<int> ConfirmNoShow(int confirmed);

// ---- The frame hook's part (UX.cpp) ----
// At every boundary, after the name tags and before the toggled patch groups: the header at an
// alone boundary or one SetHeaderFree allows (first runs only), then the locks (first runs and
// re-runs).
// `plugged`: the ports in play at `frame` (the session's synced plan, Events::PortInfo): written
// into the block while it locks, for the ready timer (a pure function of synced data, as the name
// tags are).
void OnFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating, bool alone,
             u8 plugged);
// Test knob ORCA_TEST_QUEUE=casual|ranked[:<coin 0|1>][:q2|:solo] (a test override): the header a
// queue room's host writes, in a harness run or a dev game's room, as if this game hosted such a
// room; the coin is game 1's first striker (port 1 by default). `q2`: the Slippi-style character
// select (Queue.h, FLAG_QUEUE2); `solo`: the queue's own character select before a match
// (FLAG_SOLO | FLAG_QUEUE2).
struct TestQueue
{
  Mode mode = Mode::None;
  u8 coin = 0;
  u8 flags = 0;
  bool operator==(const TestQueue&) const = default;
};
std::optional<TestQueue> ParseTestQueue(const std::string& spec);
std::optional<TestQueue> TestQueueSpec();
std::optional<Mode> TestQueueMode();
}  // namespace Rules
}  // namespace Orca::UX
