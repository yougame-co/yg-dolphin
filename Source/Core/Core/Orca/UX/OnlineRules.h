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
//   gate's masks (no B on the character select, so no way back to the menus; no B on the stage
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
  bool Locked() const { return present && mode != Mode::None && ruleset != Ruleset::None; }
  bool operator==(const Header&) const = default;
};
Header ReadHeader(const GuestMemory& memory);

// The ruleset the running profile plays: Brawl rev 2's, Project+'s, or none (no locks).
Ruleset ProfileRuleset();

// ---- The header, at an alone boundary ----
// Writes the header for `mode` (None: clears it, and gives back the rules saved when it was
// written). Entering a mode from none saves the player's own rules first. Returns the bytes
// changed (0: it already said so).
int WriteHeader(GuestMemory& memory, Mode mode, Ruleset ruleset);

// ---- The locks, at every boundary ----
// Returns the bytes changed (0 without a header that locks).
int ApplyLocks(GuestMemory& memory);

// What the input gate masks for the frame about to run (reads only).
Rollback::InputGate::Masks GateMasks(const GuestMemory& memory);

// Brawl's Random Stage Switch for the legal stages (Supernova 2025): the Brawl page's mask
// (record menu data +0x24) and the Melee page's (+0x20), without Final Destination when
// `no_final_destination`.
constexpr u32 BRAWL_LEGAL_BRAWL_PAGE = 0x04005007;  // BF, FD, Delfino, YI, Lylat, Smashville
constexpr u32 BRAWL_LEGAL_MELEE_PAGE = 0x00020000;  // Pokemon Stadium
constexpr u32 BRAWL_FINAL_DESTINATION_BIT = 0x00000002;

// The CSS characters this module knows (the select's ids, which the match's init data and result
// records use too).
constexpr int CHARACTER_ICE_CLIMBERS = 0x10;

// One port on the character select, as ApplyLocks reads it.
struct CssPort
{
  bool human = false;     // a human player's panel (not CPU, not closed)
  int character = -1;     // the character picked, -1 none yet
};
std::array<CssPort, 4> ReadCss(const GuestMemory& memory);

// The ranked character select's no-ready limit, in frames (60 s).
constexpr int CSS_READY_FRAMES = 60 * 60;

// ---- The session's side (Rollback/OnlineMatch.cpp, CPU thread) ----
// The mode this machine's room calls for, set at every boundary before the frame hook: a queue
// room's while this machine hosts it (or waits in a ranked one for the set's verdict), else none.
void SetWanted(Mode mode);
Mode Wanted();
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
void OnFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating, bool alone);
// Registers the input gate's source (UX::Init) and removes it (UX::Shutdown).
void Install();
void Uninstall();

// Test knob ORCA_TEST_QUEUE=casual|ranked (a test override): the header a queue room's host
// writes, in a harness run or a dev game's room, as if this game hosted such a room.
std::optional<Mode> TestQueueMode();
}  // namespace Rules
}  // namespace Orca::UX
