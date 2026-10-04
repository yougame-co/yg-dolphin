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
// Phase 4): on the character select of game 2 and on, the previous game's winner picks first, and
// the loser, who sees that pick, picks second. Each pick has 45 seconds; a pick that runs out keeps
// the character the player's token is on, which is the previous game's character unless they moved
// it. Project+'s 2024 Recommended order (B.6: characters, winner then loser, then the stage).
//
// How it plays, on the game's own character select (no new screen):
//   - first, the winner's turn: the loser's controller does nothing, the winner can't take the token
//     back (B) or leave (Start), and locks the pick in with A (on a character, or anywhere to keep
//     the one the token is on);
//   - then the loser's turn: the winner's controller does nothing, the loser can't take the token
//     back, and locks in with A or goes on with Start;
//   - then Orca presses Start for the loser (every other frame until the screen goes), so the game
//     goes on to the stage select with both picks. A player who picked the token up (A on it) and
//     let the time run out can only move and put it down, and Orca puts it on the character under
//     the hand, if any, before it presses Start.
// The overlay says whose turn it is and the time left ("Ada picks first · 0:42").
//
// Everything is a pure function of emulated memory and the hook's frame number, the same on both
// machines and on every re-run: the state lives in the match block (kArea), written only from the
// frame hook (idempotent: a re-run at the same boundary finds it written and writes nothing new),
// and the input gate's masks and presses are computed from that memory (Rollback/InputGate.h).
//
// Brawl keeps the interim order until its stage pick moves before characters (Supernova 2025
// §3.3: the ban, the loser's stage, then the winner may change character, then the loser): both
// pick at once on later games, with the same 45 seconds; neither can take the token back or go on
// with Start, each locks in with A (and then can't change), and once both have, or the time is up,
// Orca presses Start. MkFreePicker() is the exact order's Meta Knight re-check, for when it lands.
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

// What the match block's set state says about the game about to be played: a ranked set that is
// still open, the game number (one more than the games won: a tied game is played again), and the
// last decided game's winner. ReadSet() is the one place that reads it.
struct SetView
{
  Ruleset ruleset = Ruleset::None;  // None: no ranked set on this game; nothing here acts
  int game = 0;                     // 1-based: the game about to be played
  int last_winner = -1;             // the previous game's winning port (0 or 1), from game 2 on
  bool operator==(const SetView&) const = default;
};

// One port's part of the character select at a frame boundary.
constexpr int kNoCharacter = 0x28;

struct CssPort
{
  bool human = false;  // a human has joined this port's area
  int character = kNoCharacter;  // under the hand while the token is in it, else the token's
  bool placed = false;  // its token is on a character
  u16 pressed = 0;      // PAD_* bits newly pressed on the frame that just ran (the game's view)
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
  First = 1,   // Project+: the previous game's winner picks
  Second = 2,  // Project+: the loser picks
  Done = 3,    // both picked: Orca presses Start for the loser until the screen goes
  Both = 4,    // Brawl (interim): both pick at once, each locking in with A
};

// The order's state, in the match block at kArea.
struct State
{
  u8 game = 0;  // the set's game it belongs to (0: none)
  Step step = Step::Idle;
  u8 first = 0;      // the port that picks first (the previous game's winner)
  u8 locked = 0;     // Both: bit p set once port p locked in
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
// FreeSpace::kMatchBlock.begin; the header as every rules track writes it, the set state as the
// ranked sets track lays it out, its MatchBlock.h).
namespace Block
{
constexpr u32 MAGIC = 0x00;  // u32 "YGM1"
constexpr u32 MAGIC_VALUE = 0x59474D31;
constexpr u32 VERSION = 0x04;  // u8 1
constexpr u32 MODE = 0x05;     // u8: 2 ranked
constexpr u32 RULESET = 0x06;  // u8: 1 Brawl (Supernova 2025), 2 Project+ (2024)
constexpr u32 SET_WINS = 0x21;         // u8[2]: games won by ports 1 and 2
constexpr u32 SET_DONE = 0x23;         // u8: 0 the set is open
constexpr u32 SET_LAST_WINNER = 0x25;  // u8: the last decided game's winning port, 0xFF none
constexpr u8 MODE_RANKED = 2;
}  // namespace Block

// The step machine: the state after this boundary, from the state before it. Pure. On the
// character select of a ranked set's game 2 or later it starts the order (Project+: the winner
// first; Brawl: both at once), moves on when the player whose turn it is locks in (A newly pressed
// on a frame of the step, with the token on a character) or the pick's time is up; it clears a
// stale order on the character select of any game it doesn't apply to; off the character select it
// changes nothing.
State Advance(const SetView& set, const CssView& css, const State& state, int frame);

// The gate's masks and presses for the frame about to run, from the state the hook left. Pure.
Rollback::InputGate::Masks Gate(const SetView& set, const CssView& css, const State& state);

// The overlay's line ("Ada picks first · 0:42"; "Your pick · 0:42 · A locks it in"), empty when
// nothing is on. `ports` names the players and says which is this machine's (UI only).
std::string Line(const SetView& set, const CssView& css, const State& state,
                 const std::vector<Events::PortInfo>& ports);

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

// Test knob ORCA_TEST_CHAR_ORDER=<pplus|brawl>:<game>:<winner port 1-2> (a test override, both
// processes alike): that set view on every character select, with no match block behind it, so
// `pplus:2:1` runs the order on the boot's character select once both players have joined. Parsed
// for tests.
bool ParseTestSpec(const std::string& spec, SetView* out);
}  // namespace Orca::UX::CharOrder
