// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/RankedSteps.h"

#include <bit>

namespace Orca::UX::RankedSteps
{
namespace
{
int Count(u16 bits)
{
  return std::popcount(bits);
}

int First(u16 bits)
{
  return bits == 0 ? -1 : std::countr_zero(bits);
}

// The current step's allowed stages (see Allowed), for a step `step`.
u16 AllowedFor(const Ruleset& rules, const Plan& plan, const Step& step, const Progress& progress,
               const SetView& set)
{
  const u16 legal = rules.Legal();
  const u16 open = static_cast<u16>(legal & ~progress.struck & ~plan.excluded);
  switch (step.kind)
  {
  case Kind::Strike:
    return static_cast<u16>(open & rules.starters);
  case Kind::Ban:
  case Kind::Pick:
  {
    if (plan.mk_clause)
      return open;
    if (set.last_winner < 0)
      return static_cast<u16>(open & rules.starters);  // game 1's last starter
    // Full DSR: the picker (the previous game's loser) may not pick a stage it has won on.
    const int loser = 1 - set.last_winner;
    const u16 dsr = static_cast<u16>(open & ~set.won[loser]);
    if (dsr != 0)
      return dsr;
    if (open != 0)
      return open;
    return static_cast<u16>(legal & ~plan.excluded) != 0 ?
               static_cast<u16>(legal & ~plan.excluded) :
               legal;
  }
  default:
    return 0;
  }
}

void Enter(const Plan& plan, Progress* progress, u8 step)
{
  progress->step = step;
  progress->done = 0;
  progress->frames =
      step < plan.count ? static_cast<u16>(plan.steps[step].frames + GRACE_FRAMES) : u16(0);
}

// After a strike or ban: the step ends once its count is made, or when one more would leave
// nothing to play (a strike never takes the last starter, a ban never the picker's last stage).
void AfterStrike(const Ruleset& rules, const Plan& plan, Progress* progress, const SetView& set)
{
  const Step& step = plan.steps[progress->step];
  ++progress->done;
  if (progress->done >= step.count || Count(AllowedFor(rules, plan, step, *progress, set)) <= 1)
    Enter(plan, progress, static_cast<u8>(progress->step + 1));
  AutoPick(rules, plan, progress, set);
}
}  // namespace

const Ruleset& BrawlSupernova2025()
{
  static const Ruleset rules = [] {
    Ruleset r;
    r.stages = BRAWL_STAGES;
    r.starters = (1 << Battlefield) | (1 << PokemonStadium) | (1 << LylatCruise) |
                 (1 << Smashville) | (1 << YoshisIsland);
    r.fd = FinalDestination;
    r.later_bans = 1;
    r.later_bans_optional = true;
    r.meta_knight = BRAWL_META_KNIGHT;
    r.ice_climbers = BRAWL_ICE_CLIMBERS;
    return r;
  }();
  return rules;
}

u8 CoinFromSeed(u32 seed)
{
  // A full avalanche of the word (murmur3's finalizer), so the coin is not the LCG's low bit.
  u32 h = seed;
  h ^= h >> 16;
  h *= 0x85EBCA6B;
  h ^= h >> 13;
  h *= 0xC2B2AE35;
  h ^= h >> 16;
  return static_cast<u8>(h >> 31);
}

Plan PlanGame(const Ruleset& rules, const SetView& set, u8 coin, std::array<int, PORTS> characters)
{
  Plan plan;
  const auto is = [](int character, int id) { return id >= 0 && character == id; };
  if (rules.fd >= 0 &&
      (is(characters[0], rules.ice_climbers) || is(characters[1], rules.ice_climbers)))
  {
    plan.excluded = static_cast<u16>(1u << rules.fd);
  }
  const bool mk0 = is(characters[0], rules.meta_knight);
  const bool mk1 = is(characters[1], rules.meta_knight);
  if (mk0 != mk1)
  {
    // The MK clause: the player who isn't Meta Knight picks any legal stage, over strikes, bans
    // and DSR.
    plan.mk_clause = true;
    plan.steps[0] = {Kind::Pick, static_cast<u8>(mk0 ? 1 : 0), 1, rules.pick_frames, false};
    plan.count = 1;
    return plan;
  }
  coin &= 1;
  if (set.last_winner < 0)
  {
    // Game 1: 1-2-1 on the starters, the coin's port first; the last starter is the stage.
    const u8 other = static_cast<u8>(1 - coin);
    plan.steps[0] = {Kind::Strike, coin, 1, rules.strike_frames[0], false};
    plan.steps[1] = {Kind::Strike, other, 2, rules.strike_frames[1], false};
    plan.steps[2] = {Kind::Strike, coin, 1, rules.strike_frames[2], false};
    plan.steps[3] = {Kind::Pick, coin, 1, rules.pick_frames, false};
    plan.count = 4;
    return plan;
  }
  // Later games: the winner bans (or strikes), the loser picks with full DSR.
  const u8 winner = static_cast<u8>(set.last_winner & 1);
  const u8 loser = static_cast<u8>(1 - winner);
  plan.steps[0] = {Kind::Ban, winner, rules.later_bans, rules.ban_frames,
                   rules.later_bans_optional};
  plan.steps[1] = {Kind::Pick, loser, 1, rules.pick_frames, false};
  plan.count = 2;
  return plan;
}

Progress Begin(const Plan& plan)
{
  Progress progress;
  Enter(plan, &progress, 0);
  return progress;
}

const Step* Current(const Plan& plan, const Progress& progress)
{
  if (progress.picked >= 0 || progress.step >= plan.count)
    return nullptr;
  return &plan.steps[progress.step];
}

u16 Allowed(const Ruleset& rules, const Plan& plan, const Progress& progress, const SetView& set)
{
  const Step* step = Current(plan, progress);
  return step ? AllowedFor(rules, plan, *step, progress, set) : u16(0);
}

bool Strike(const Ruleset& rules, const Plan& plan, Progress* progress, const SetView& set,
            int stage)
{
  const Step* step = Current(plan, *progress);
  if (!step || (step->kind != Kind::Strike && step->kind != Kind::Ban) || stage < 0 ||
      stage >= rules.stages)
  {
    return false;
  }
  const u16 allowed = AllowedFor(rules, plan, *step, *progress, set);
  if (!(allowed & (1u << stage)) || Count(allowed) <= 1)
    return false;
  progress->struck = static_cast<u16>(progress->struck | (1u << stage));
  AfterStrike(rules, plan, progress, set);
  return true;
}

bool Skip(const Plan& plan, Progress* progress)
{
  const Step* step = Current(plan, *progress);
  if (!step || !step->optional)
    return false;
  Enter(plan, progress, static_cast<u8>(progress->step + 1));
  return true;
}

bool Pick(const Ruleset& rules, const Plan& plan, Progress* progress, const SetView& set, int stage)
{
  const Step* step = Current(plan, *progress);
  if (!step || step->kind != Kind::Pick || stage < 0 || stage >= rules.stages)
    return false;
  if (!(AllowedFor(rules, plan, *step, *progress, set) & (1u << stage)))
    return false;
  progress->picked = static_cast<s8>(stage);
  return true;
}

void AutoPick(const Ruleset& rules, const Plan& plan, Progress* progress, const SetView& set)
{
  const Step* step = Current(plan, *progress);
  if (!step || step->kind != Kind::Pick)
    return;
  const u16 allowed = AllowedFor(rules, plan, *step, *progress, set);
  if (Count(allowed) == 1)
    progress->picked = static_cast<s8>(First(allowed));
}

void Tick(const Ruleset& rules, const Plan& plan, Progress* progress, const SetView& set)
{
  const Step* step = Current(plan, *progress);
  if (!step)
    return;
  if (progress->frames > 0)
    --progress->frames;
  if (progress->frames > 0)
    return;
  switch (step->kind)
  {
  case Kind::Strike:
  case Kind::Ban:
    if (step->optional)
    {
      Skip(plan, progress);
      AutoPick(rules, plan, progress, set);
      return;
    }
    // The first allowed stages in list order, until the step is made.
    for (int guard = 0; guard < MAX_STAGES && Current(plan, *progress) == step; ++guard)
    {
      const int stage = First(AllowedFor(rules, plan, *step, *progress, set));
      if (stage < 0 || !Strike(rules, plan, progress, set, stage))
      {
        Enter(plan, progress, static_cast<u8>(progress->step + 1));
        AutoPick(rules, plan, progress, set);
        break;
      }
    }
    return;
  case Kind::Pick:
  {
    const int stage = First(AllowedFor(rules, plan, *step, *progress, set));
    progress->picked = static_cast<s8>(stage >= 0 ? stage : 0);
    return;
  }
  default:
    return;
  }
}

int SecondsShown(u16 frames)
{
  if (frames <= GRACE_FRAMES)
    return 0;
  return (frames - GRACE_FRAMES + 59) / 60;
}
}  // namespace Orca::UX::RankedSteps
