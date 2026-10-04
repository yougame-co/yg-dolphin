// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// A ranked set's stage steps (RankedSteps.h): Brawl under Supernova 2025.

#include <gtest/gtest.h>

#include "Core/Orca/UX/RankedSteps.h"

using namespace Orca::UX::RankedSteps;

namespace
{
const Ruleset& R()
{
  return BrawlSupernova2025();
}

constexpr int MARIO = 0;
constexpr int LINK = 2;

u16 Bit(int stage)
{
  return static_cast<u16>(1u << stage);
}

// Runs a step's timer out.
void RunOut(const Plan& plan, Progress* p, const SetView& set)
{
  for (int i = 0; i < 100000 && Current(plan, *p) && p->picked < 0; ++i)
  {
    const u8 step = p->step;
    Tick(R(), plan, p, set);
    if (p->step != step)
      return;
  }
}
}  // namespace

TEST(OrcaRankedSteps, CoinIsFairAndDeterministic)
{
  int ones = 0;
  for (u32 seed = 0; seed < 20000; ++seed)
    ones += CoinFromSeed(seed * 0x41C64E6D + 12345);
  EXPECT_GT(ones, 9500);
  EXPECT_LT(ones, 10500);
  EXPECT_EQ(CoinFromSeed(0xDEADBEEF), CoinFromSeed(0xDEADBEEF));
}

TEST(OrcaRankedSteps, GameOneStrikesOneTwoOneThenTheLastStarter)
{
  const SetView set;
  for (u8 coin = 0; coin < 2; ++coin)
  {
    const Plan plan = PlanGame(R(), set, coin, {MARIO, LINK});
    ASSERT_EQ(plan.count, 4);
    EXPECT_FALSE(plan.mk_clause);
    EXPECT_EQ(plan.steps[0].port, coin);
    EXPECT_EQ(plan.steps[1].port, 1 - coin);
    EXPECT_EQ(plan.steps[2].port, coin);
    Progress p = Begin(plan);
    EXPECT_EQ(Allowed(R(), plan, p, set), R().starters);
    // Counterpicks can't be struck in game 1.
    EXPECT_FALSE(Strike(R(), plan, &p, set, FinalDestination));
    EXPECT_FALSE(Strike(R(), plan, &p, set, DelfinoPlaza));
    EXPECT_TRUE(Strike(R(), plan, &p, set, Battlefield));
    EXPECT_EQ(p.step, 1);
    EXPECT_FALSE(Strike(R(), plan, &p, set, Battlefield));  // already struck
    EXPECT_TRUE(Strike(R(), plan, &p, set, Smashville));
    EXPECT_EQ(p.step, 1);
    EXPECT_TRUE(Strike(R(), plan, &p, set, LylatCruise));
    EXPECT_EQ(p.step, 2);
    EXPECT_EQ(p.frames, R().strike_frames[2] + GRACE_FRAMES);
    EXPECT_TRUE(Strike(R(), plan, &p, set, YoshisIsland));
    // The last starter is the stage, picked by the steps.
    EXPECT_EQ(p.picked, PokemonStadium);
    EXPECT_EQ(Current(plan, p), nullptr);
  }
}

TEST(OrcaRankedSteps, StrikeTimersStrikeTheFirstStartersInListOrder)
{
  const SetView set;
  const Plan plan = PlanGame(R(), set, 0, {MARIO, LINK});
  Progress p = Begin(plan);
  EXPECT_EQ(SecondsShown(p.frames), 30);
  RunOut(plan, &p, set);
  EXPECT_EQ(p.struck, Bit(Battlefield));
  EXPECT_EQ(p.step, 1);
  RunOut(plan, &p, set);
  EXPECT_EQ(p.struck, Bit(Battlefield) | Bit(PokemonStadium) | Bit(LylatCruise));
  EXPECT_EQ(SecondsShown(p.frames), 10);
  RunOut(plan, &p, set);
  EXPECT_EQ(p.picked, YoshisIsland);
}

TEST(OrcaRankedSteps, TimerShowsZeroThroughTheGrace)
{
  EXPECT_EQ(SecondsShown(GRACE_FRAMES + 30 * 60), 30);
  EXPECT_EQ(SecondsShown(GRACE_FRAMES + 29 * 60 + 1), 30);
  EXPECT_EQ(SecondsShown(GRACE_FRAMES + 1), 1);
  EXPECT_EQ(SecondsShown(GRACE_FRAMES), 0);
  EXPECT_EQ(SecondsShown(1), 0);
}

TEST(OrcaRankedSteps, LaterGamesWinnerMayBanThenLoserPicksWithFullDsr)
{
  SetView set;
  set.game = 3;
  set.last_winner = 1;  // port 2 won game 2
  set.won[0] = Bit(PokemonStadium);
  set.won[1] = Bit(Smashville);
  const Plan plan = PlanGame(R(), set, 0, {MARIO, LINK});
  ASSERT_EQ(plan.count, 2);
  EXPECT_EQ(plan.steps[0].kind, Kind::Ban);
  EXPECT_EQ(plan.steps[0].port, 1);
  EXPECT_TRUE(plan.steps[0].optional);
  EXPECT_EQ(plan.steps[1].kind, Kind::Pick);
  EXPECT_EQ(plan.steps[1].port, 0);
  Progress p = Begin(plan);
  // Port 1 won on Pokemon Stadium: it may not pick it, so there is nothing to ban there.
  const u16 open = static_cast<u16>(R().Legal() & ~Bit(PokemonStadium));
  EXPECT_EQ(Allowed(R(), plan, p, set), open);
  EXPECT_FALSE(Strike(R(), plan, &p, set, PokemonStadium));
  EXPECT_TRUE(Strike(R(), plan, &p, set, DelfinoPlaza));
  EXPECT_EQ(p.step, 1);
  EXPECT_EQ(Allowed(R(), plan, p, set), open & ~Bit(DelfinoPlaza));
  EXPECT_FALSE(Pick(R(), plan, &p, set, DelfinoPlaza));
  EXPECT_FALSE(Pick(R(), plan, &p, set, PokemonStadium));
  EXPECT_TRUE(Pick(R(), plan, &p, set, FinalDestination));
  EXPECT_EQ(p.picked, FinalDestination);
}

TEST(OrcaRankedSteps, TheBanIsOptional)
{
  SetView set;
  set.game = 2;
  set.last_winner = 0;
  set.won[0] = Bit(Battlefield);
  const Plan plan = PlanGame(R(), set, 1, {MARIO, LINK});
  Progress p = Begin(plan);
  EXPECT_TRUE(Skip(plan, &p));
  EXPECT_EQ(p.step, 1);
  EXPECT_EQ(p.struck, 0);
  // The pick isn't optional.
  EXPECT_FALSE(Skip(plan, &p));
  // A ban's timer skips it; a pick's picks the first allowed stage.
  Progress q = Begin(plan);
  RunOut(plan, &q, set);
  EXPECT_EQ(q.step, 1);
  EXPECT_EQ(q.struck, 0);
  RunOut(plan, &q, set);
  EXPECT_EQ(q.picked, Battlefield);  // port 2 lost and has won nowhere
}

TEST(OrcaRankedSteps, MetaKnightClause)
{
  // Game 1: exactly one Meta Knight, the other player picks any legal stage, no strikes.
  SetView set;
  Plan plan = PlanGame(R(), set, 0, {BRAWL_META_KNIGHT, LINK});
  ASSERT_EQ(plan.count, 1);
  EXPECT_TRUE(plan.mk_clause);
  EXPECT_EQ(plan.steps[0].kind, Kind::Pick);
  EXPECT_EQ(plan.steps[0].port, 1);
  Progress p = Begin(plan);
  EXPECT_EQ(Allowed(R(), plan, p, set), R().Legal());
  EXPECT_TRUE(Pick(R(), plan, &p, set, DelfinoPlaza));

  // A later game: over the ban and DSR, whoever won.
  set.game = 2;
  set.last_winner = 0;
  set.won[0] = Bit(FinalDestination);
  plan = PlanGame(R(), set, 0, {MARIO, BRAWL_META_KNIGHT});
  ASSERT_EQ(plan.count, 1);
  EXPECT_EQ(plan.steps[0].port, 0);
  p = Begin(plan);
  EXPECT_TRUE(Pick(R(), plan, &p, set, FinalDestination));

  // Both Meta Knight: the normal steps.
  plan = PlanGame(R(), set, 0, {BRAWL_META_KNIGHT, BRAWL_META_KNIGHT});
  EXPECT_FALSE(plan.mk_clause);
  EXPECT_EQ(plan.count, 2);
}

TEST(OrcaRankedSteps, IceClimbersNeverOnFinalDestination)
{
  SetView set;
  set.game = 2;
  set.last_winner = 1;
  Plan plan = PlanGame(R(), set, 0, {BRAWL_ICE_CLIMBERS, LINK});
  EXPECT_EQ(plan.excluded, Bit(FinalDestination));
  Progress p = Begin(plan);
  EXPECT_FALSE(Allowed(R(), plan, p, set) & Bit(FinalDestination));
  EXPECT_FALSE(Strike(R(), plan, &p, set, FinalDestination));
  ASSERT_TRUE(Skip(plan, &p));
  EXPECT_FALSE(Pick(R(), plan, &p, set, FinalDestination));
  // The MK clause too.
  plan = PlanGame(R(), set, 0, {BRAWL_META_KNIGHT, BRAWL_ICE_CLIMBERS});
  ASSERT_TRUE(plan.mk_clause);
  p = Begin(plan);
  EXPECT_EQ(Allowed(R(), plan, p, set), R().Legal() & ~Bit(FinalDestination));
}

TEST(OrcaRankedSteps, DsrNeverLeavesNothing)
{
  SetView set;
  set.game = 6;
  set.last_winner = 1;
  set.won[0] = R().Legal();  // a long set: port 1 has won everywhere
  const Plan plan = PlanGame(R(), set, 0, {MARIO, LINK});
  Progress p = Begin(plan);
  EXPECT_EQ(Allowed(R(), plan, p, set), R().Legal());
}

TEST(OrcaRankedSteps, ADrawReplaysTheSameSteps)
{
  // The set view only changes on a decided game, so a draw plans the game again the same way.
  const SetView set;
  EXPECT_EQ(PlanGame(R(), set, 1, {MARIO, LINK}), PlanGame(R(), set, 1, {MARIO, LINK}));
}
