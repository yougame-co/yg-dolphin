// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

// A ranked set's stage steps (ORCA.md "Ranked stage flow"), shared by Brawl (Supernova 2025) and
// Project+ (2024 Recommended procedure, 2024 Proposed stage list). Pure logic: no memory, no
// state. Each game's adapter (RankedPPlus.h for Project+; Brawl's track for vanilla) keeps the set
// and step state in the match block (MatchBlock.h), asks here which steps a game has and what the
// player whose turn it is may do, and maps the ruleset's stage indices to its own stage select.
//
// Stages are the ruleset's legal list, starters first: bit i of a StageMask is legal stage i.
//
// Game 1: the coin's port strikes 1, the other strikes 2, then the coin's port picks one of the
// last two (the same as striking one of them: "1-2-1, and the first striker picks from the last
// two"). Later games: the last game's winner strikes (Project+: 2) or may ban (Brawl: 1, optional)
// from the whole list, then the loser picks, never a stage they already won on (full DSR).
// Brawl's Meta Knight clause: when exactly one player is Meta Knight, the other picks any legal
// stage (no strikes, bans or DSR), game 1 included. Brawl's Ice Climbers rule: Final Destination
// is never available while either player is Ice Climbers.
namespace Orca::UX::Ranked
{
using StageMask = u16;
constexpr int MAX_STAGES = 16;

enum class StepKind : u8
{
  Strike = 1,
  Ban = 2,
  Pick = 3,
};

struct Step
{
  StepKind kind = StepKind::Pick;
  u8 port = 0;            // 0 or 1: the player whose turn it is
  u8 count = 1;           // stages to strike or ban (1 for a pick)
  bool optional = false;  // a ban the player may leave out (its timer runs out with no ban)
  u16 seconds = 30;       // the turn timer
  bool operator==(const Step&) const = default;
};

struct Ruleset
{
  u8 id = 0;  // MatchBlock's ruleset byte
  std::string_view name;
  int stage_count = 0;
  StageMask starters = 0;
  StageMask counterpicks = 0;
  int wins_needed = 2;  // best of 3
  // Later games: the winner's step.
  StepKind later_kind = StepKind::Strike;
  u8 later_count = 2;
  bool later_optional = false;
  bool dsr = true;  // full DSR: the loser never picks a stage they won a game on
  // Brawl: the stage Ice Climbers never play on (-1: none), and the characters (gmGlobalModeMelee
  // character kinds, Results.h) the clauses look for (-1: no such clause).
  int ic_banned_stage = -1;
  int ice_climbers = -1;
  int meta_knight = -1;
  // Turn timers, seconds: game 1's three steps, a later game's strike or ban, a later pick.
  std::array<u16, 3> game1_seconds{30, 30, 10};
  u16 later_seconds = 30;
  u16 pick_seconds = 30;
  // Every timer also has this much grace before its default applies (shown as 0:00 meanwhile).
  u16 grace_frames = 180;
  // The stages' short names, for the overlay and logs.
  std::array<std::string_view, MAX_STAGES> stage_names{};

  StageMask All() const { return static_cast<StageMask>((1u << stage_count) - 1); }
};

// MatchBlock's ruleset ids.
constexpr u8 RULESET_NONE = 0;
constexpr u8 RULESET_BRAWL_SUPERNOVA_2025 = 1;
constexpr u8 RULESET_PPLUS_2024 = 2;

// Project+ 2024: BF, PS2, SV, LM, ToT; counterpicks GHZ, BC, FH, DL. Winner strikes 2.
const Ruleset& PPlus2024();
// Brawl, Supernova 2025: BF, PS1 (Melee page), LC, SV, YI; counterpicks FD, Delfino. Winner may
// ban 1. Meta Knight clause; Ice Climbers never on FD.
const Ruleset& Brawl2025();
// By MatchBlock id; nullptr for none or unknown.
const Ruleset* RulesetById(u8 id);

// What a game's steps depend on.
struct SetView
{
  int game = 1;          // 1-based; a draw replays the same game number
  int last_winner = -1;  // port 0 or 1 of the last counted game; -1 before any
  int coin = 0;          // game 1's first striker (port 0 or 1), the fair coin in the match block
  std::array<StageMask, 2> won_on{};      // the stages each port won a game on
  std::array<int, 2> characters{-1, -1};  // gmGlobalModeMelee character kinds; -1 unknown
  bool operator==(const SetView&) const = default;
};

struct Plan
{
  std::vector<Step> steps;
  // In play at the start of the game's steps (the rest is shown struck the whole time).
  StageMask available = 0;
  // Also never pickable in the pick step (DSR), on top of the strikes.
  StageMask pick_excluded = 0;
  // Brawl's Meta Knight clause decided this game: one pick of any legal stage.
  bool mk_clause = false;
};

Plan PlanGame(const Ruleset& rules, const SetView& set);

// The stages the player at `step` may act on, given what was struck or banned so far this game.
StageMask Allowed(const Plan& plan, int step, StageMask struck);
// What a step's timer does when it runs out: the strikes or bans still owed (`done` of them made),
// the first allowed stages in list order (an optional ban: none); for a pick, the first allowed
// stage. 0 when nothing is allowed.
StageMask DefaultStrikes(const Plan& plan, int step, StageMask struck, int done);
int DefaultPick(const Plan& plan, int step, StageMask struck);
// A step's timer in frames, grace included.
u32 StepFrames(const Ruleset& rules, const Step& step);

int CountBits(StageMask mask);
// The lowest `n` set bits of `mask`.
StageMask FirstBits(StageMask mask, int n);

// ---- A game's steps driven one action and one frame at a time ----
// For a stage select whose glue sees each strike, ban, skip and pick as it happens and counts the
// screen's own frames (Brawl's, BrawlStages.h): where the steps stand, kept in the match block. The
// same rules as above (Allowed, DefaultStrikes, DefaultPick, StepFrames); Project+'s glue keeps its
// own deadline instead (RankedPPlus.h).
struct Progress
{
  u8 step = 0;           // index into the plan; plan.steps.size(): done
  u8 done = 0;           // strikes or bans made in this step
  StageMask struck = 0;  // stages struck or banned this game
  u16 frames = 0;        // left in this step's timer, grace included
  s8 picked = -1;        // the stage picked (-1: not yet)
  bool operator==(const Progress&) const = default;
};

// A game's first step, its timer started (and the pick made when only one stage is left).
Progress Begin(const Ruleset& rules, const Plan& plan);
// The step under way; nullptr once the stage is picked or the steps are done.
const Step* Current(const Plan& plan, const Progress& progress);
// The stages the current step allows; 0 when there is none.
StageMask Allowed(const Plan& plan, const Progress& progress);
// The current step's player strikes or bans `stage`: false (nothing changes) unless the step is a
// strike or a ban that allows it, and it isn't the last stage standing. A step whose count is made
// ends, and a pick with one stage left makes itself.
bool Strike(const Ruleset& rules, const Plan& plan, Progress* progress, int stage);
// The current step's player leaves an optional ban out. False if it isn't optional.
bool Skip(const Ruleset& rules, const Plan& plan, Progress* progress);
// The current step's player picked `stage` (the game took it). False if the step doesn't allow it.
bool Pick(const Plan& plan, Progress* progress, int stage);
// A pick step with exactly one stage allowed picks it.
void AutoPick(const Plan& plan, Progress* progress);
// One frame of the current step's timer; when it runs out, its default: the strikes still owed
// (DefaultStrikes), an optional ban left out, the first allowed stage picked (DefaultPick).
void Tick(const Ruleset& rules, const Plan& plan, Progress* progress);
// Whole seconds a step's timer shows (0 through the grace).
int SecondsShown(const Ruleset& rules, u16 frames);
}  // namespace Orca::UX::Ranked
