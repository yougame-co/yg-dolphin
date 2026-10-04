// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Rollback/InputGate.h"

namespace Core
{
class CPUThreadGuard;
}

// Project+'s ranked stage flow (ORCA.md "Ranked stage flow"), on its own stage select: the 2024
// Recommended procedure on the 2024 Proposed stage list, with the steps of RankedSteps.h and the
// set kept in the match block (MatchBlock.h).
//
// - Outside the stage select, the 2024 Proposed preset (Switch03.rss) is in RSS_EXDATA: page 0 is
//   the 9 legal stages, hazards off. The game builds its stage select from it.
// - On the stage select, Project+'s own Stage Striking does the striking: the player whose turn it
//   is presses X on a stage (the hook at 0x806B586C, untouched), the game marks it in
//   STAGE_STRIKE_TABLE and draws its struck art. At each boundary the frame hook counts the new
//   strikes into the block, then writes the table the block calls for: stages not in play this
//   game (game 1's counterpicks, the loser's won stages) and every stage of the other pages shown
//   struck, so Project+'s own check refuses them. A change it makes is redrawn by Project+'s code
//   (PAGE_INDEX set apart from CURRENT_PAGE).
// - Turns: the stage select's controller field (+0x278) is the active port, and the input gate
//   leaves only that port's stick and the step's button (X to strike, A to pick); B, L, R, Z,
//   Start, Y and the C-stick are masked (no clearing strikes, no L+R+A back to the characters, no
//   alternate stages, no hazards), as are A and X on Random and the items past it, and on the
//   stage builder's page.
// - Timers: a strike step that runs out strikes the first remaining stages in list order; a pick
//   that runs out picks the first allowed stage (the selection set to it and A pressed through the
//   gate).
// - Results: the results screen's winner of a fight started from the flow counts (Results.h
//   Judge); a draw or a void doesn't. Two wins take the set.
//
// Integration (ranked-sprint): the header is OnlineRules' (with the coin); the set's score here is
// this flow's own count of the results screens of fights it started, which agrees with the results
// track's set (SetBlock.h) for Project+ (both take the results screen's winner; ties count for
// neither); the input gate ORs these masks with every other rule's (UX.cpp).
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::RankedPPlus
{
// Project+ v3.2's stage select data (Source/Netplay/Net-Random.asm).
constexpr u32 RSS_EXDATA = 0x8042C4E8;
constexpr u32 RSS_EXDATA_SIZE = 0x320;
// What the flow writes of the preset: every preset Project+ ships is the same after it.
constexpr u32 PRESET_BYTES = 0xA8;
constexpr u32 PAGE_INDEX = 0x8042C821;
constexpr u32 STAGE_STRIKE_TABLE = 0x8042C822;
constexpr u32 STRIKE_PAGES = 5;
constexpr u32 CURRENT_PAGE = 0x80496000;
// The stage select task's fields (Brawl's muSelectStageTask, the same in Project+).
constexpr u32 SSS_HOVERED = 0x244;     // the item under the cursor (RANDOM_ITEM: Random)
constexpr u32 SSS_SELECTED = 0x248;    // the page position selected, -1 none
constexpr u32 SSS_CONTROLLER = 0x278;  // 0xF0 every port, 0-3 that port only
constexpr u32 SSS_PAGE = 0x228;        // Brawl's page: CUSTOM_PAGE the stage builder's
constexpr u32 CUSTOM_PAGE = 2;
// The hovered item's values: 0 nothing, 1-0x34 a stage, then these.
constexpr u32 PAGE_ITEM = 0x35;    // the page button
constexpr u32 RANDOM_ITEM = 0x36;  // Random

// The 2024 Proposed preset, Project+ v3.2's pf/stage/switch/Switch03.rss.
extern const u8 kSwitch03[RSS_EXDATA_SIZE];

// The ruleset's stage for each Project+ stage kind (the kinds the preset's page 0 lists, through
// its slot table at RSS_EXDATA+0x104), or -1.
int LegalIndexOfKind(int kind);
// The preset's page 0 as the game holds it: for each position, the ruleset's stage (-1: none).
std::vector<int> PageZero(const GuestMemory& memory);

// The frame hook's part, first runs and re-runs alike (a pure, idempotent function of memory,
// `frame` and whether two players are plugged in): returns how many bytes changed.
int Apply(GuestMemory& memory, int frame, bool two_players);
// The input gate's masks for the frame about to run (reads only).
Rollback::InputGate::Masks Masks(const GuestMemory& memory);

// What the overlay shows (UI only): the set's line on the menus, and on the stage select whose
// turn it is and its timer. Empty strings: nothing.
struct Lines
{
  std::string set;
  std::string turn;
  bool operator==(const Lines&) const = default;
};
Lines Describe(const GuestMemory& memory, int frame, const std::vector<Events::PortInfo>& ports);

// The whole frame hook part (flow, overlay lines). The header is OnlineRules' (MatchBlock.h).
void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports, bool alone);
// The overlay's lines, as the frame hook last published them. Any thread.
Lines CurrentLines();
}  // namespace Orca::UX::RankedPPlus
