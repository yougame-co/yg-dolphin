// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "Common/CommonTypes.h"

// A ranked set's stage steps, pure logic (no memory, no session): who strikes, bans or picks, how
// many, what is allowed, the turn timers and their defaults. The Brawl stage select glue
// (BrawlStages.h) stores a Progress in the match block and drives it from the game's memory, so
// every machine and every re-run computes the same steps.
//
// MERGE NOTE (sp-brawl-sss): this is the Brawl track's own minimal step machine, written before
// the Project+ track's shared one was pushed. It is ruleset-driven (Ruleset below) so it can absorb
// P+'s list (2 strikes in later games, not optional) or be replaced by the shared one.
namespace Orca::UX::RankedSteps
{
constexpr int MAX_STAGES = 16;
constexpr int PORTS = 2;
// A turn timer's grace: the timer shows 0:00 for this long before its default applies.
constexpr u16 GRACE_FRAMES = 3 * 60;

struct Ruleset
{
  u8 stages = 0;      // the legal stages, in list order (timer defaults take the first)
  u16 starters = 0;   // game 1's strike list (bits of the list)
  int fd = -1;        // the stage Ice Climbers may not play (-1: no IC/FD rule)
  u8 later_bans = 1;  // what the previous game's winner strikes or bans in later games
  bool later_bans_optional = true;
  int meta_knight = -1;   // the character the MK clause names (-1: no clause)
  int ice_climbers = -1;  // (-1: no IC/FD rule)
  // Timers in frames (60 a second), before the grace.
  std::array<u16, 3> strike_frames{30 * 60, 30 * 60, 10 * 60};
  u16 ban_frames = 30 * 60;
  u16 pick_frames = 30 * 60;

  u16 Legal() const { return static_cast<u16>((1u << stages) - 1); }
};

// The legal stages of Brawl under Supernova 2025, in list order.
enum BrawlStage : u8
{
  Battlefield,
  PokemonStadium,  // the Melee page's
  LylatCruise,
  Smashville,
  YoshisIsland,  // Brawl's
  FinalDestination,
  DelfinoPlaza,
  BRAWL_STAGES,
};
// Brawl's character ids as gmGlobalModeMelee and the result info hold them.
constexpr int BRAWL_ICE_CLIMBERS = 0x10;
constexpr int BRAWL_META_KNIGHT = 0x18;
const Ruleset& BrawlSupernova2025();

enum class Kind : u8
{
  None = 0,
  Strike = 1,  // game 1: strike starters
  Ban = 2,     // later games: the winner's ban(s) or strikes
  Pick = 3,    // the stage is picked (by a player, or by the steps when one is left)
};

struct Step
{
  Kind kind = Kind::None;
  u8 port = 0;   // 0 or 1: whose turn
  u8 count = 0;  // strikes or bans to make (Pick: 1)
  u16 frames = 0;
  bool optional = false;  // may be skipped (Brawl's ban)
  bool operator==(const Step&) const = default;
};

struct Plan
{
  std::array<Step, 4> steps{};
  u8 count = 0;
  // No stage this game for anybody: IC/FD.
  u16 excluded = 0;
  // The MK clause decided this game: one pick of any legal stage.
  bool mk_clause = false;
  bool operator==(const Plan&) const = default;
};

// What the set knows between games.
struct SetView
{
  u8 game = 1;                   // 1-based
  s8 last_winner = -1;           // port 0 or 1 of the previous decided game; -1 in game 1
  std::array<u16, PORTS> won{};  // the stages each port has won on this set (full DSR)
};

// Game 1's first striker: a fair coin from a word of the game's own state (its RNG seed), the same
// on both machines and nobody's port.
u8 CoinFromSeed(u32 seed);

// A game's steps. `characters` are ports 1 and 2's (-1 unknown).
Plan PlanGame(const Ruleset& rules, const SetView& set, u8 coin, std::array<int, PORTS> characters);

// Where a game's stage steps stand (the match block keeps it).
struct Progress
{
  u8 step = 0;     // index into the plan; plan.count: done
  u8 done = 0;     // strikes or bans made in this step
  u16 struck = 0;  // stages struck or banned this game
  u16 frames = 0;  // left in this step, grace included
  s8 picked = -1;  // the stage picked (-1: not yet)
  bool operator==(const Progress&) const = default;
};

// A new game's progress: the first step's timer.
Progress Begin(const Plan& plan);
const Step* Current(const Plan& plan, const Progress& progress);
// The stages the current step allows (strike, ban or pick), 0 when done.
u16 Allowed(const Ruleset& rules, const Plan& plan, const Progress& progress, const SetView& set);

// The active port strikes or bans `stage`. False (nothing changes) if the step doesn't allow it.
bool Strike(const Ruleset& rules, const Plan& plan, Progress* progress, const SetView& set,
            int stage);
// The active port skips an optional step. False if it isn't optional.
bool Skip(const Plan& plan, Progress* progress);
// The active port picked `stage` (the game took it). False if not allowed.
bool Pick(const Ruleset& rules, const Plan& plan, Progress* progress, const SetView& set,
          int stage);
// One frame of the current step's timer; when it runs out, its default: the first allowed stages
// struck, an optional ban skipped, the first allowed stage picked.
void Tick(const Ruleset& rules, const Plan& plan, Progress* progress, const SetView& set);
// A pick step with exactly one stage left picks it.
void AutoPick(const Ruleset& rules, const Plan& plan, Progress* progress, const SetView& set);

// Whole seconds a step's timer shows (0 through the grace).
int SecondsShown(u16 frames);
}  // namespace Orca::UX::RankedSteps
