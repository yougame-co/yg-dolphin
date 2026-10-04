// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/FreeSpace.h"
#include "Core/Rollback/InputGate.h"

namespace Core
{
class CPUThreadGuard;
}

// The character order of a ranked set's later games (ORCA.md "Character order"; the ranked rules'
// Phase 4), on the character select of game 2 and on, in one of two orders (Order, one constant
// for both games, kLaterGameOrder):
//   - WinnerFirst, the one played (Luis, 2026-10-04: "Winner chooses character first", then the
//     loser; the standard rulesets too: Project+'s 2024 Recommended B.6, characters winner then
//     loser, then the stage): the winner picks first, and the loser, who sees that pick, second.
//   - Free (Luis's earlier pick, 2026-10-04: "losing player chooses character first then winning
//     player chooses. Winning player can choose right away if they want"): the loser is asked
//     first; the winner may lock in at any time meanwhile, and if they haven't once the loser has,
//     it is their turn.
// Each turn has 45 seconds; a pick that runs out keeps the character the player's token is on,
// which is the previous game's character unless they moved it.
//
// How it plays, on the game's own character select (no new screen; Luis, 2026-10-04: "press A to
// select character and then press start after to lock in and ready up"):
//   - on a player's turn, A on a character puts the token there (a pick), B with the token down
//     takes it back up to pick again (the online rules let B reach the game only for that, never
//     as the held B that backs out to the menus, OnlineRules.h CssBUnpicks), and Start with the
//     token down locks the pick in: final, the turn is over. Start is never the game's here (the
//     input gate keeps it; the raw latch, Queue.h RAW, shows it to the order);
//   - first the first picker's turn; WinnerFirst: the other player's controller does nothing.
//     Free: the winner may pick and lock in too (then does nothing more);
//   - then the second picker's turn (unless they locked in already, Free): the first picker's
//     controller does nothing;
//   - then the stage select: in a queue2 room the queue goes on at once (Queue.h, no ready step
//     after the order); else Orca presses Start for the second picker (every other frame until
//     the screen goes). A player whose time ran out with the token in the hand can only move and
//     put it down, and Orca puts it on the character under the hand, if any.
// The overlay says whose turn it is, what to do and the time left ("ada locked in · bo: pick and
// press Start · 0:42").
//
// Orca 0.3.14 and 0.3.15 locked a pick in with A anywhere in the grid and masked B: a player whose
// token was still down from the last game (the tokens stay down from one character select to the
// next) and who pressed A on another character to take it locked the old one in instead, and their
// controller then did nothing at all (Luis's "stuck", 2026-10-04).
//
// Everything is a pure function of emulated memory and the hook's frame number, the same on both
// machines and on every re-run: the state lives in the match block (kArea), written only from the
// frame hook (idempotent: a re-run at the same boundary finds it written and writes nothing new),
// and the input gate's masks and presses are computed from that memory (Rollback/InputGate.h).
//
// Brawl plays the same order as Project+ (Supernova 2025's exact one, §3.3: the ban, the loser's
// stage, then the winner may change character, then the loser, needs the stage pick before the
// characters). MkFreePicker() is that order's Meta Knight re-check, for when it lands.
//
// Found under the harness (ORCA_UX_PROBE watches and dumps, Brawl rev 2 and Project+ alike):
//   - the character select: scene "scSelctCharacter", whose +0x400 is muSelCharTask; player area i
//     (GameCube port i) at task +0x44 + 4 * i:
//       +0x10 u32 the buttons newly pressed on the frame that just ran (PAD_* bits; +0x0C held),
//             as the game saw them (after the input gate);
//       +0x1B4 the player kind (1: a human has joined);
//       +0x1B8 the character under the hand while the token is in it, else the token's
//             (kNoCharacter: none);
//       +0x1F8 u8 1 while the token is in the hand, 0 once it is down (+0x1F9 1 while it flies);
//   - A on a character with the token in the hand puts it there; A on the player's own token picks
//     it up; B takes it back to the hand; A elsewhere with the token down changes nothing; the
//     tokens stay down from one game's character select to the next (and back from the stage
//     select).
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::CharOrder
{
// Which ruleset a ranked set plays (the match block's header; ORCA.md "Online rules").
enum class Ruleset : u8
{
  None = 0,
  Brawl = 1,  // Supernova 2025
  PPlus = 2,  // Project+ 2024 Recommended
};

// Who picks first on a later game (above).
enum class Order : u8
{
  Free = 0,         // the loser is asked first; the winner may lock in at any time
  WinnerFirst = 1,  // the winner locks in first, then the loser picks
};
// The order both games play (Luis, 2026-10-04: the winner chooses first). Flip it here.
constexpr Order kLaterGameOrder = Order::WinnerFirst;

// What the match block's set state says about the game about to be played: a ranked set that is
// still open, the game number (one more than the games won: a tied game is played again), and the
// last decided game's winner. ReadSet() is the one place that reads it.
struct SetView
{
  Ruleset ruleset = Ruleset::None;  // None: no ranked set on this game; nothing here acts
  int game = 0;                     // 1-based: the game about to be played
  int last_winner = -1;             // the previous game's winning port (0 or 1), from game 2 on
  Order order = kLaterGameOrder;
  bool operator==(const SetView&) const = default;
};
// The port that is asked first: the winner (WinnerFirst) or the loser (Free). -1 without a winner.
int FirstPicker(const SetView& set);

// One port's part of the character select at a frame boundary.
constexpr int kNoCharacter = 0x28;

struct CssPort
{
  bool human = false;  // a human has joined this port's area
  int character = kNoCharacter;  // under the hand while the token is in it, else the token's
  bool placed = false;  // its token is out of the hand (on a character, or on its way down)
  bool flying = false;  // the token is on its way up or down
  // The player's raw buttons (PAD_* bits) as the latch kept them (Queue.h RAW, RAW_PREV): held on
  // the frame that just ran, and newly pressed on it. The game never sees Start here.
  u16 raw = 0;
  u16 raw_new = 0;
  // On a character and still: Start locks it in, B takes it up.
  bool Down() const { return human && placed && !flying && character != kNoCharacter; }
  bool operator==(const CssPort&) const = default;
};
struct CssView
{
  bool on_css = false;
  std::array<CssPort, 2> ports{};
  bool operator==(const CssView&) const = default;
};

enum class Step : u8
{
  Idle = 0,    // no order on this game
  First = 1,   // the first picker's turn (Free: the winner may lock in meanwhile)
  Second = 2,  // the second picker's turn
  Done = 3,    // both picked: Orca presses Start for the second picker until the screen goes
};

// The order's state, in the match block at kArea.
struct State
{
  u8 game = 0;  // the set's game it belongs to (0: none)
  Step step = Step::Idle;
  u8 first = 0;      // the port that picks first (FirstPicker)
  u8 locked = 0;     // Free: bit p set once port p locked in
  u32 since = 0;     // the hook frame its step began at
  u16 elapsed = 0;   // frames into the step, as of the last boundary (what the gate reads)
  bool operator==(const State&) const = default;
};

// 45 seconds a pick (the ranked rules' character timers), at the game's 60 frames a second.
constexpr int PICK_FRAMES = 45 * 60;

// The order's 16 bytes, right after the match block (FreeSpace.h): the same dead code of
// sqNetAnyOkiraku::setNext, short of kMatchBlockLimit, so the rules tracks' own layouts of the
// 512-byte block can't collide with it. +0 kMagic, +4 game, +5 step, +6 first, +7 locked, +8 since,
// +0xC elapsed, +0xE 0. Anything without the magic reads as Idle.
constexpr u32 kArea = FreeSpace::kMatchBlock.end;
constexpr u32 kAreaSize = 0x10;
constexpr u32 kMagic = 0x5947434F;  // "YGCO"
static_assert(kArea % 16 == 0 && kArea + kAreaSize <= FreeSpace::kMatchBlockLimit &&
              FreeSpace::Detail::Free({kArea, kArea + kAreaSize}));

// The match block's header and the ranked set's state, as ReadSet() reads them (offsets from
// FreeSpace::kMatchBlock.begin; the header as OnlineRules writes it, MatchBlock.h; the set as the
// ranked set track keeps it, SetBlock.h).
namespace Block
{
constexpr u32 MAGIC = 0x00;  // u32 "YGM1"
constexpr u32 MAGIC_VALUE = 0x59474D31;
constexpr u32 VERSION = 0x04;  // u8 1
constexpr u32 MODE = 0x05;     // u8: 2 ranked
constexpr u32 RULESET = 0x06;  // u8: 1 Brawl (Supernova 2025), 2 Project+ (2024)
// The ranked set's (SetBlock.h, +0x210).
constexpr u32 SET_WINS = 0x211;         // u8[2]: games won by ports 1 and 2
constexpr u32 SET_DONE = 0x213;         // u8: 0 the set is open
constexpr u32 SET_LAST_WINNER = 0x215;  // u8: the last decided game's winning port, 0xFF none
constexpr u8 MODE_RANKED = 2;
}  // namespace Block

// The step machine: the state after this boundary, from the state before it. Pure. On the
// character select of a ranked set's game 2 or later it starts the order (FirstPicker first), moves
// on when the player whose turn it is locks in (Start newly pressed on a frame of the step, raw,
// with the token down on a character) or the pick's time is up (Free: past the second picker's
// turn when they locked in already); it clears a stale order on the character select of any game
// it doesn't apply to; off the character select it changes nothing.
State Advance(const SetView& set, const CssView& css, const State& state, int frame);

// The gate's masks and presses for the frame about to run, from the state the hook left. Pure.
Rollback::InputGate::Masks Gate(const SetView& set, const CssView& css, const State& state);

// The overlay's line ("ada picks first · 0:42"; "ada locked in · bo: pick and press Start ·
// 0:31"), empty when nothing is on. `ports` names the players and says which is this machine's (UI
// only).
std::string Line(const SetView& set, const CssView& css, const State& state,
                 const std::vector<Events::PortInfo>& ports);

// Which ports have locked in (bit p) on this state: the first picker's once its turn is over
// (Free: the winner's own lock too), both once the order is done. 0 while the order isn't on.
// Display only (the overlay's LOCKED IN badges).
u8 LockedPorts(const SetView& set, const CssView& css, const State& state);

// Supernova 2025's Meta Knight clause, re-checked once both final characters are known (Brawl's
// exact order): if exactly one player is Meta Knight, the other port picks any legal stage. -1:
// the clause doesn't apply. `meta_knight` is Meta Knight's id in the space `characters` uses (the
// stage tracks' gmGlobalModeMelee id, or the character select's).
int MkFreePicker(const std::array<int, 2>& characters, int meta_knight);

// ---- Memory ----

SetView ReadSet(const GuestMemory& memory);
CssView ReadCss(const GuestMemory& memory);
State ReadState(const GuestMemory& memory);
// Writes `state` at kArea, only the bytes that differ. Returns how many bytes changed.
int WriteState(GuestMemory& memory, const State& state);

// The frame hook's part (UX.cpp): reads, advances and writes the state (first runs and re-runs),
// and on first runs gives the overlay its line.
void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports);
// The input gate's part (Rollback::InputGate::SetSource): masks from memory alone.
Rollback::InputGate::Masks GateFrame(const Core::CPUThreadGuard& guard);

// Test knob ORCA_TEST_CHAR_ORDER=<pplus|brawl>:<game>:<winner port 1-2>[:free|winnerfirst] (a test
// override, both processes alike): that set view on every character select, with no match block
// behind it, so `pplus:2:1` runs the order on the boot's character select once both players have
// joined; the order is kLaterGameOrder unless given. Parsed for tests.
bool ParseTestSpec(const std::string& spec, SetView* out);
// Whether ORCA_TEST_CHAR_ORDER is on (the latch then keeps the raw buttons on its character select
// too, Queue.h).
bool TestActive();
}  // namespace Orca::UX::CharOrder
