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
//
// Casual games (PlanCasual, ORCA.md "Casual stage pick"): no strikes. Both players name a
// preferred stage from the whole legal list at the same time (one step for both, casual_seconds;
// a player who named none when it runs out gets a random legal stage). The same stage starts the
// match at once; two different ones, a coin from the room and the game number takes one of them.
//
// Agreement (ranked, Luis 2026-10-04): either player may propose a stage at any time (Propose):
// a legal stage in play this game that isn't struck or banned (DSR ignored). When both players'
// proposals are the same stage, that is the stage: the rest of the steps are skipped.
namespace Orca::UX::Ranked
{
using StageMask = u16;
constexpr int MAX_STAGES = 16;

enum class StepKind : u8
{
  Strike = 1,
  Ban = 2,
  Pick = 3,
  // A casual game's preferred stage: A on any allowed stage names it, and the turn passes on.
  Prefer = 4,
};

// A step's port when it is both players' at once (a casual game's Prefer).
constexpr u8 BOTH = 2;

struct Step
{
  StepKind kind = StepKind::Pick;
  u8 port = 0;            // 0 or 1: the player whose turn it is (BOTH: a casual Prefer)
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
  // A casual game's preferred stages (one Prefer step, both players at once): its seconds and its
  // own short grace (Luis, 2026-10-04: "Players have 15 seconds to select stage").
  u16 casual_seconds = 15;
  u16 casual_grace_frames = 30;
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
  // A casual game's (PlanCasual): the coin its random choices come from (CasualSeed).
  bool casual = false;
  u32 seed = 0;
  // The stages the two players may agree on (Propose): legal, in play this game (game 1: the
  // starters), DSR ignored. Strikes and bans this game come off it as they are made.
  StageMask agreeable = 0;
};

Plan PlanGame(const Ruleset& rules, const SetView& set);

// ---- Casual games ----
// The coin a casual game's random choices come from: the room code's hash (MatchBlock ROOM), the
// header's coin and the game number (1-based; a fight from the stage select counts one), mixed
// (murmur3's finalizer), so neither player can tell which way it falls, and both machines, every
// re-run and a drop-in's keyframe compute the same.
u32 CasualSeed(u32 room, u8 coin, int game);
// A casual game's steps: one Prefer step for both players at once, on the whole legal list
// (Brawl: never Final Destination with an Ice Climbers; no Meta Knight clause).
Plan PlanCasual(const Ruleset& rules, const SetView& set, u32 seed);
// The stage a Prefer turn names when its timer runs out: a legal stage at random (the plan's seed
// and the port). -1 when nothing is allowed.
int CasualDefault(const Plan& plan, int port);
// Whose preferred stage the match is played on (0 or 1) when they differ: one bit of the plan's
// seed.
int CasualChoice(const Plan& plan);
// A casual game's stage from both preferred stages (-1 for one not named: the timer's default for
// that port): the same stage, or the coin's (CasualChoice) of two different ones.
int CasualResult(const Plan& plan, std::array<s8, 2> prefs);

// The stages the player at `step` may act on, given what was struck or banned so far this game.
StageMask Allowed(const Plan& plan, int step, StageMask struck);
// What a step's timer does when it runs out: the strikes or bans still owed (`done` of them made),
// the first allowed stages in list order (an optional ban: none); for a pick, the first allowed
// stage. 0 when nothing is allowed.
StageMask DefaultStrikes(const Plan& plan, int step, StageMask struck, int done);
int DefaultPick(const Plan& plan, int step, StageMask struck);
// A step's grace (Prefer's own, else the ruleset's), and its timer in frames, grace included.
u16 GraceFrames(const Ruleset& rules, const Step& step);
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
  // Each port's proposed stage (-1: none). A casual game's preferred stages: once both are named,
  // `picked` is CasualResult's. Ranked: once both are the same stage, `picked` is that stage.
  std::array<s8, 2> prefs{-1, -1};
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
// `port` (0 or 1) proposes `stage` (a casual game: names its preferred stage). False (nothing
// changes) once the stage is picked, for a stage the players can't agree on (Agreeable; a casual
// game's: Allowed), or the stage it already proposed. Then: a casual game with both named picks
// CasualResult's; a ranked game whose two proposals are the same stage picks it.
bool Propose(const Plan& plan, Progress* progress, int port, int stage);
// `port` takes its proposal back. False if it had none (or the stage is picked).
bool Withdraw(Progress* progress, int port);
// What `port` may propose now: Agreeable (ranked) or Allowed (a casual game's Prefer step); 0 once
// the stage is picked.
StageMask Proposable(const Plan& plan, const Progress& progress);
// A pick step with exactly one stage allowed picks it.
void AutoPick(const Plan& plan, Progress* progress);
// One frame of the current step's timer; when it runs out, its default: the strikes still owed
// (DefaultStrikes), an optional ban left out, the first allowed stage picked (DefaultPick); in a
// casual game, a random legal stage named for each player who named none (CasualDefault), then
// CasualResult's stage.
void Tick(const Ruleset& rules, const Plan& plan, Progress* progress);
// Whole seconds a step's timer shows (0 through the grace). The first form is for the ruleset's
// own grace (strikes, bans, picks).
int SecondsShown(const Ruleset& rules, u16 frames);
int SecondsShown(const Ruleset& rules, const Step& step, u16 frames);
}  // namespace Orca::UX::Ranked
