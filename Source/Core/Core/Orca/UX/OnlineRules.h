// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>

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
//   is its player's alone (no peer plugged in or on the way, nothing anyone replays). Anything
//   else (a friends room, solo play, a search still going, the queue room left) clears it at the
//   next such boundary, giving back the player's own rules saved when it was written. A ranked
//   room keeps it while the set is open, whoever is plugged in. A joiner checks it once its
//   keyframe is in: another mode or ruleset than its own queue's leaves the room ("mismatch").
// - The locks, while the header says casual or ranked: the ruleset's rules (stocks, time, items
//   off, handicap off, team attack on, ratio 1.0, pause off), the stage choice (casual: Random,
//   from the legal stages; ranked: Choose), the legal list (Brawl's Random Stage Switch without
//   Final Destination while someone is Ice Climbers; Project+'s 2024 Proposed list), and the input
//   gate's masks (no B on the character select, so no way back to the menus, and A only in the
//   character grid, so no player-type, name, team, rules or BACK button; no B on the stage
//   select; no Start in a fight, so no pause; Project+'s D-pad on the character select (its Code
//   Menu) and L, R and Z on the stage select (alternate stages, hazards, L+R+A)).
// - Ranked's character select timer: once one player has a character and the other hasn't, 60 s;
//   before the set's first fight, the one who still hasn't is a no-show and forfeits.
// Everything but the header is a pure function of emulated memory (the block included), written
// only where it differs, on first runs and re-runs alike: the same on both machines, idempotent.
// Friends rooms have no header, so nothing here touches their games.
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

// ---- The locks, at every boundary ----
// `frame`: the frame hook's (the ranked ready timer counts from it). Returns the bytes changed (0
// without a header that locks).
int ApplyLocks(GuestMemory& memory, int frame);

// What the input gate masks for the frame about to run (reads only).
Rollback::InputGate::Masks GateMasks(const GuestMemory& memory);

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
// A passes only strictly between these (one frame's travel inside the grid's edges, both games).
constexpr float CSS_A_TOP = 15.0f;
constexpr float CSS_A_BOTTOM = -3.0f;

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
// Whether A may reach the game from this hand while the locks hold. An unreadable hand may not.
bool CssHandMayPressA(const CssHand& hand);

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
// The mode this machine's room calls for, set at every boundary before the frame hook: a queue
// room's while this machine hosts it (or waits in a ranked one for the set's verdict), the queue's
// own character select's (FLAG_SOLO, with the app's `queue2`) while its player waits there for a
// match, else none. `flags`: MatchBlock::FLAG_*.
void SetWanted(Mode mode, u8 flags = 0);
Mode Wanted();
u8 WantedFlags();
// Whether the last alone boundary left the header as Wanted() says (a keyframe for a queue room's
// opponent waits for it).
bool HeaderSettled();
// A joiner whose keyframe just loaded, into a room of this queue: the header is checked at the
// next frame hook.
void ExpectHeader(Mode mode);
// "mismatch" once the check failed: the joiner leaves.
std::optional<std::string> TakeMismatch();
// The ranked no-show, once its frame is final: the port (0-based) that didn't ready in time.
// `confirmed`: frames through it are final (OnlineMatch's ConfirmResults).
std::optional<int> ConfirmNoShow(int confirmed);

// ---- The frame hook's part (UX.cpp) ----
// At every boundary, after the name tags and before the toggled patch groups: the header at an
// alone boundary (first runs only), then the locks (first runs and re-runs).
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
