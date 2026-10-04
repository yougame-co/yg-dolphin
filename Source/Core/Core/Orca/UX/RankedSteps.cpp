// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/RankedSteps.h"

#include <algorithm>

namespace Orca::UX::Ranked
{
namespace
{
constexpr StageMask Bit(int i)
{
  return static_cast<StageMask>(1u << i);
}

Ruleset MakePPlus()
{
  Ruleset r;
  r.id = RULESET_PPLUS_2024;
  r.name = "Project+ 2024";
  r.stage_count = 9;
  r.starters = 0x001F;      // BF, PS2, SV, LM, ToT
  r.counterpicks = 0x01E0;  // GHZ, BC, FH, DL
  r.later_kind = StepKind::Strike;
  r.later_count = 2;
  r.later_optional = false;
  r.stage_names = {"Battlefield",     "Pokémon Stadium 2", "Smashville",
                   "Luigi's Mansion", "Temple of Time",    "Green Hill Zone",
                   "Bowser's Castle", "Frigate Husk",      "Dream Land"};
  return r;
}

Ruleset MakeBrawl()
{
  Ruleset r;
  r.id = RULESET_BRAWL_SUPERNOVA_2025;
  r.name = "Brawl Supernova 2025";
  r.stage_count = 7;
  r.starters = 0x001F;      // BF, PS1, LC, SV, YI
  r.counterpicks = 0x0060;  // FD, Delfino
  r.later_kind = StepKind::Ban;
  r.later_count = 1;
  r.later_optional = true;
  r.ic_banned_stage = 5;  // Final Destination
  r.ice_climbers = 0x10;  // gmCharacterKind (BrawlHeaders gm_lib.h)
  r.meta_knight = 0x18;
  r.stage_names = {"Battlefield",    "Pokémon Stadium",   "Lylat Cruise", "Smashville",
                   "Yoshi's Island", "Final Destination", "Delfino Plaza"};
  return r;
}
}  // namespace

const Ruleset& PPlus2024()
{
  static const Ruleset r = MakePPlus();
  return r;
}

const Ruleset& Brawl2025()
{
  static const Ruleset r = MakeBrawl();
  return r;
}

const Ruleset* RulesetById(u8 id)
{
  if (id == RULESET_PPLUS_2024)
    return &PPlus2024();
  if (id == RULESET_BRAWL_SUPERNOVA_2025)
    return &Brawl2025();
  return nullptr;
}

int CountBits(StageMask mask)
{
  int n = 0;
  for (; mask; mask &= static_cast<StageMask>(mask - 1))
    ++n;
  return n;
}

StageMask FirstBits(StageMask mask, int n)
{
  StageMask out = 0;
  for (int i = 0; i < MAX_STAGES && n > 0; ++i)
  {
    if (mask & Bit(i))
    {
      out |= Bit(i);
      --n;
    }
  }
  return out;
}

Plan PlanGame(const Ruleset& rules, const SetView& set)
{
  Plan plan;
  const StageMask all = rules.All();
  StageMask legal = all;
  const auto is = [&](int port, int character) {
    return character >= 0 && set.characters[port] == character;
  };
  if (rules.ic_banned_stage >= 0 && (is(0, rules.ice_climbers) || is(1, rules.ice_climbers)))
  {
    legal &= static_cast<StageMask>(~Bit(rules.ic_banned_stage));
  }
  const int coin = set.coin == 1 ? 1 : 0;
  // Meta Knight clause: exactly one Meta Knight; the other player picks any legal stage.
  if (rules.meta_knight >= 0 && is(0, rules.meta_knight) != is(1, rules.meta_knight))
  {
    const u8 other = is(0, rules.meta_knight) ? 1 : 0;
    plan.available = legal;
    plan.steps.push_back({StepKind::Pick, other, 1, false, rules.pick_seconds});
    return plan;
  }
  if (set.game <= 1 || (set.last_winner != 0 && set.last_winner != 1))
  {
    // Game 1 (or a set with no counted game yet): 1-2-1 on the starters, the last one a pick.
    plan.available = rules.starters & legal;
    const u8 first = static_cast<u8>(coin);
    const u8 second = static_cast<u8>(1 - coin);
    const int n = CountBits(plan.available);
    // 5 starters: strike 1, strike 2, pick of the last two. Fewer (Ice Climbers' FD as a starter
    // in another ruleset) keeps the order, each step no larger than what leaves one to pick.
    const int s1 = std::min(1, std::max(0, n - 1));
    const int s2 = std::min(2, std::max(0, n - 1 - s1));
    if (s1 > 0)
      plan.steps.push_back(
          {StepKind::Strike, first, static_cast<u8>(s1), false, rules.game1_seconds[0]});
    if (s2 > 0)
      plan.steps.push_back(
          {StepKind::Strike, second, static_cast<u8>(s2), false, rules.game1_seconds[1]});
    plan.steps.push_back({StepKind::Pick, first, 1, false, rules.game1_seconds[2]});
    return plan;
  }
  const u8 winner = static_cast<u8>(set.last_winner);
  const u8 loser = static_cast<u8>(1 - set.last_winner);
  plan.available = legal;
  if (rules.dsr)
    plan.available &= static_cast<StageMask>(~set.won_on[loser]);
  // Never more strikes than leave one stage to pick.
  const int n = CountBits(plan.available);
  const int count = std::min<int>(rules.later_count, std::max(0, n - 1));
  if (count > 0)
  {
    plan.steps.push_back({rules.later_kind, winner, static_cast<u8>(count), rules.later_optional,
                          rules.later_seconds});
  }
  plan.steps.push_back({StepKind::Pick, loser, 1, false, rules.pick_seconds});
  return plan;
}

StageMask Allowed(const Plan& plan, int step, StageMask struck)
{
  if (step < 0 || step >= static_cast<int>(plan.steps.size()))
    return 0;
  StageMask allowed = plan.available & static_cast<StageMask>(~struck);
  if (plan.steps[step].kind == StepKind::Pick)
    allowed &= static_cast<StageMask>(~plan.pick_excluded);
  return allowed;
}

StageMask DefaultStrikes(const Plan& plan, int step, StageMask struck, int done)
{
  if (step < 0 || step >= static_cast<int>(plan.steps.size()))
    return 0;
  const Step& s = plan.steps[step];
  if (s.kind == StepKind::Pick || s.optional)
    return 0;
  const StageMask allowed = Allowed(plan, step, struck);
  // Never strike the last stage standing.
  const int owed = std::min(s.count - done, CountBits(allowed) - 1);
  return owed > 0 ? FirstBits(allowed, owed) : 0;
}

int DefaultPick(const Plan& plan, int step, StageMask struck)
{
  const StageMask allowed = Allowed(plan, step, struck);
  for (int i = 0; i < MAX_STAGES; ++i)
  {
    if (allowed & Bit(i))
      return i;
  }
  return -1;
}

u32 StepFrames(const Ruleset& rules, const Step& step)
{
  return static_cast<u32>(step.seconds) * 60 + rules.grace_frames;
}
}  // namespace Orca::UX::Ranked
