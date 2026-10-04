// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/RankedSteps.h"
#include "Core/Rollback/InputGate.h"

namespace Core
{
class CPUThreadGuard;
}

// A ranked set's stage flow on vanilla Brawl's own stage select (RSBE01 rev 2, Supernova 2025;
// ORCA.md "Ranked stages (Brawl)"): game 1's 1-2-1 strikes on the five starters, the first striker
// a coin from the game's own RNG; later games' optional ban by the winner, then the loser's pick
// with full DSR; the MK clause; IC/FD; turns, turn timers and their defaults; struck and illegal
// stages greyed and refused; the last stage selected by the flow.
//
// Everything is a pure function of emulated memory: the set and the steps live in the match block
// (FreeSpace.h), the frame hook writes them from what the game shows (the scene, the stage
// select's cursor, the pads the game read), and the input gate's source (Masks) reads them back.
// Every write is idempotent at its boundary (a hook run again there, after a rollback's load,
// changes nothing), and the turn timer counts the game's own frames, so both machines, every re-run
// and a drop-in's keyframe compute the same.
//
// MERGE NOTES (sp-brawl-sss):
// - Whether this game plays a ranked Brawl set comes from the match block's header, which the
//   casual track (sp-casual) writes. Until it lands, only the test knob ORCA_TEST_RANKED=brawl
//   turns the flow on (RankedBrawlMatch); production never activates it.
// - The set state (games, score, last winner, stages won) is kept here, in this flow's own region
//   of the block (kRegion). If the results track's set state lands, read it instead, and clear this
//   region wherever the header's alone rule clears the set.
// - The step machine is this track's own (RankedSteps.h).
// - The input gate has one source: UX.cpp registers one that ORs every rule's masks; this flow's
//   are BrawlStages::Masks.
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::BrawlStages
{
// ---- The game's memory (Brawl rev 2) ----
constexpr u32 SCENE_MANAGER = 0x805A0060;  // gfSceneManager*: +4 the scene
constexpr u32 PAD_SYSTEM = 0x805A0040;     // gfPadSystem*
constexpr u32 RNG_SEED = 0x805A00BC;       // g_mtRandDefault's seed
constexpr u32 GAME_GLOBAL = 0x805A00E0;    // g_GameGlobal*
// gfPadSystem: the game's pads as it read them, 4 x 0x40 from +0x40; +0 the buttons held.
constexpr u32 PAD_SYSTEM_PADS = 0x40;
constexpr u32 PAD_STRIDE = 0x40;
// gfPadButtons bits.
constexpr u16 GF_X = 0x0400;
constexpr u16 GF_Y = 0x0800;
// The stage select scene (scSelStage) +0x3AC: its task.
constexpr u32 SCENE_SSS_TASK = 0x3AC;
// The task: +0x8C the stage icons (u32 pointers), +0x244 the hovered item, +0x248 the selected
// slot (-1 none), +0x278 the port that drives the cursor (0xF0 every port).
constexpr u32 TASK_ICONS = 0x8C;
constexpr u32 TASK_HOVER = 0x244;
constexpr u32 TASK_SELECTED = 0x248;
constexpr u32 TASK_PORT = 0x278;
constexpr u32 PORT_ANY = 0xF0;

// ---- This flow's region of the match block ----
constexpr u32 kRegion = 0x806F2500;
constexpr u32 kRegionSize = 0x80;
constexpr u32 kMagic = 0x59474253;  // 'YGBS'
constexpr u8 kVersion = 1;

enum class Phase : u8
{
  Idle = 0,      // on the character select, before a game's stage select
  Select = 1,    // this game's stage steps are under way (or done) on the stage select
  Fight = 2,     // the game is on
  Recorded = 3,  // its result is in the set
};

// What the region holds.
struct State
{
  RankedSteps::SetView set;
  std::array<u8, 2> score{};
  u8 coin = 0xFF;  // game 1's first striker; 0xFF until drawn
  Phase phase = Phase::Idle;
  RankedSteps::Progress progress;
  std::array<s8, 2> characters{-1, -1};
  std::array<u16, 2> buttons{};  // each port's buttons at the last boundary (X and Y edges)
  u32 clock = 0;                 // the stage select's frame counter at the last tick
  s8 hovered = -1;               // the stage under the cursor at the last boundary
  u8 hovered_for = 0;            // frames the cursor has rested on what it is on (A needs 1)
  u8 hover_key = 0xFF;           // what it is on: the page and the hovered item (0xFF nothing)
  bool operator==(const State&) const = default;
};
std::optional<State> ReadState(const GuestMemory& memory);
// Writes only the bytes that differ.
void WriteState(GuestMemory& memory, const State& state);
void ClearState(GuestMemory& memory);

// Where a legal stage's icon is on the stage select: its page (0 Brawl's stages, 1 Melee's), its
// slot on the page (the task's +0x248 while the cursor is on it), and a point inside it in the
// cursor's coordinates.
struct Tile
{
  u8 page;
  u8 slot;
  float x;
  float y;
};
const Tile& TileFor(int stage);

// A legal stage's id in the game's stage list (the result info's +0x0C), -1 none.
int StageForStageId(int stage_id);

// The stage select as one boundary sees it.
struct View
{
  bool present = false;  // scSelStage with its task
  u32 task = 0;
  u32 hovered_item = 0xFFFFFFFF;
  s32 selected = -1;
  u32 clock = 0;
};
View ReadView(const GuestMemory& memory);

// Whether this game is a ranked Brawl set's (see MERGE NOTES).
bool RankedBrawlMatch(const GuestMemory& memory);
// Unit tests: RankedBrawlMatch's answer instead of the knob's (nullopt: the knob again).
void SetRankedForTests(std::optional<bool> ranked);

// The frame hook's part, at every boundary (first runs and re-runs): follows the set and the
// stage steps, and writes the stage select's turn, grey tiles and the stage the steps pick.
void Frame(GuestMemory& memory);
// The input gate's part (reads only): what each port may not press on the stage select.
void Masks(const GuestMemory& memory, Rollback::InputGate::Masks* masks);
// The overlay's lines for this boundary (empty: none): the set score on the character and stage
// selects, whose turn it is and its timer on the stage select. `ports` for the names, `local` this
// machine's own port (UI only).
std::pair<std::string, std::string> Lines(const GuestMemory& memory,
                                          const std::vector<Orca::Events::PortInfo>& ports);

// The CPU thread's frame hook and gate source.
void FrameHook(const Core::CPUThreadGuard& guard, bool resimulating,
               const std::vector<Orca::Events::PortInfo>& ports);
void GateMasks(const Core::CPUThreadGuard& guard, Rollback::InputGate::Masks* masks);
}  // namespace Orca::UX::BrawlStages
