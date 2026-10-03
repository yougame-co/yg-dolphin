// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The ranked stage flow (ORCA.md "Ranked stage flow"): the shared step machine (RankedSteps.h),
// the match block (MatchBlock.h) and Project+'s flow on its stage select (RankedPPlus.h), over a
// fake of the game's memory.

#include <map>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "Core/Orca/UX/MatchBlock.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/RankedPPlus.h"
#include "Core/Orca/UX/RankedSteps.h"
#include "InputCommon/GCPadStatus.h"

using namespace Orca::UX;
using Ranked::StageMask;
using Ranked::StepKind;

namespace
{
class FakeMemory final : public GuestMemory
{
public:
  bool Valid(u32 a) const override { return bytes.contains(a); }
  u8 Read8(u32 a) const override { return bytes.at(a); }
  u16 Read16(u32 a) const override { return static_cast<u16>(Read8(a) << 8 | Read8(a + 1)); }
  u32 Read32(u32 a) const override { return u32(Read16(a)) << 16 | Read16(a + 2); }
  void Write8(u32 a, u8 v) override { bytes[a] = v; }
  void Write16(u32 a, u16 v) override
  {
    Write8(a, static_cast<u8>(v >> 8));
    Write8(a + 1, static_cast<u8>(v));
  }
  void Write32(u32 a, u32 v) override
  {
    Write16(a, static_cast<u16>(v >> 16));
    Write16(a + 2, static_cast<u16>(v));
  }
  void Fill(u32 a, u32 n, u8 v = 0)
  {
    for (u32 i = 0; i < n; ++i)
      bytes[a + i] = v;
  }
  void Text(u32 a, std::string_view s)
  {
    for (size_t i = 0; i < s.size(); ++i)
      bytes[a + static_cast<u32>(i)] = static_cast<u8>(s[i]);
    bytes[a + static_cast<u32>(s.size())] = 0;
  }
  std::map<u32, u8> bytes;
};

constexpr u32 SCENE = 0x80910000;
constexpr u32 SCENE_NAME = 0x80920000;
constexpr u32 TASK = 0x80930000;
constexpr u32 RESULT_INFO = 0x9017F420;
constexpr u32 MODE_MELEE = 0x90180F20;

// The preset's page 0 in the ruleset's order of the legal list: GHZ, BC, FH, DL, ToT, SV, BF, PS2,
// LM (Switch03.rss), so position p is legal stage POS_LEGAL[p].
constexpr int POS_LEGAL[9] = {5, 6, 7, 8, 4, 2, 0, 1, 3};
int PosOf(int legal)
{
  for (int p = 0; p < 9; ++p)
  {
    if (POS_LEGAL[p] == legal)
      return p;
  }
  return -1;
}

// Project+'s memory as RankedPPlus reads it: the scene manager and the scene's name, the stage
// select task, the preset and the strike table, the match setup and the result info, and the
// match block's dead code.
FakeMemory Game(std::string_view scene)
{
  FakeMemory m;
  m.Fill(0x805A0060, 4);
  m.Fill(0x80900000, 0x10);
  m.Fill(SCENE, 0x400);
  m.Write32(0x805A0060, 0x80900000);
  m.Write32(0x80900004, SCENE);
  m.Write32(SCENE, SCENE_NAME);
  m.Fill(SCENE_NAME, 0x20);
  m.Text(SCENE_NAME, scene);
  m.Write32(SCENE + 0x3AC, TASK);
  m.Fill(TASK, 0x300);
  m.Write32(TASK + RankedPPlus::SSS_SELECTED, 0xFFFFFFFF);
  m.Write32(TASK + RankedPPlus::SSS_CONTROLLER, 0xF0);
  // Project+'s netplay default preset in RSS_EXDATA (any other bytes will do), its tables after.
  m.Fill(RankedPPlus::RSS_EXDATA, 0x400, 0x11);
  m.Fill(RankedPPlus::STAGE_STRIKE_TABLE - 1, 0x1F, 0);
  m.Fill(RankedPPlus::CURRENT_PAGE, 4);
  m.Fill(0x805A00E0, 4);
  m.Fill(0x90181300, 0x40);
  m.Write32(0x805A00E0, 0x90181300);
  m.Write32(0x90181308, MODE_MELEE);
  m.Write32(0x90181318, RESULT_INFO);
  m.Fill(MODE_MELEE, 0x100);
  m.Fill(RESULT_INFO, 0x24 + 4 * 0x2AC + 4);
  for (u32 port = 0; port < 4; ++port)
    m.Write8(RESULT_INFO + 0x24 + port * 0x2AC + 0x01, 3);
  m.Fill(MatchBlock::BASE, 0x200, 0x3A);
  m.Write32(MatchBlock::BASE, FreeSpace::kMatchBlockOriginalWord);
  return m;
}

void SetScene(FakeMemory& m, std::string_view scene)
{
  m.Fill(SCENE_NAME, 0x20);
  m.Text(SCENE_NAME, scene);
}

// A results screen where `winner` (port 0 or 1) took the game.
void SetResult(FakeMemory& m, int winner)
{
  m.Write8(RESULT_INFO + 0x01, 1);
  m.Write16(RESULT_INFO + 0x0E, 1);
  for (u32 port = 0; port < 2; ++port)
  {
    const u32 rec = RESULT_INFO + 0x24 + port * 0x2AC;
    m.Write8(rec + 0x00, port == 0 ? 0x00 : 0x03);
    m.Write8(rec + 0x01, 0);
    m.Write8(rec + 0x0A, static_cast<int>(port) == winner ? 1 : 0);
    m.Write8(rec + 0x0E, static_cast<int>(port) == winner ? 0 : 1);
  }
}

// What Project+'s X does: the stage's bit in page 0 of the strike table.
void StrikeAt(FakeMemory& m, int legal)
{
  const u32 bits = u32(m.Read16(RankedPPlus::STAGE_STRIKE_TABLE + 2)) << 16 |
                   m.Read16(RankedPPlus::STAGE_STRIKE_TABLE + 4);
  const u32 now = bits | (1u << PosOf(legal));
  m.Write16(RankedPPlus::STAGE_STRIKE_TABLE + 2, static_cast<u16>(now >> 16));
  m.Write16(RankedPPlus::STAGE_STRIKE_TABLE + 4, static_cast<u16>(now));
}

u32 Page0Bits(const FakeMemory& m)
{
  return u32(m.Read16(RankedPPlus::STAGE_STRIKE_TABLE + 2)) << 16 |
         m.Read16(RankedPPlus::STAGE_STRIKE_TABLE + 4);
}

u32 BitsOf(StageMask legal)
{
  u32 bits = 0;
  for (int i = 0; i < 9; ++i)
  {
    if (legal & (1u << i))
      bits |= 1u << PosOf(i);
  }
  return bits;
}

MatchBlock::State Block(const FakeMemory& m)
{
  const auto s = MatchBlock::Read(m);
  EXPECT_TRUE(s.has_value());
  return s.value_or(MatchBlock::State{});
}

// A ranked Project+ set in the block, with the 2024 Proposed preset already loaded (as the frame
// hook leaves it outside the stage select).
FakeMemory RankedSss(u8 coin)
{
  FakeMemory m = Game("scSelctCharacter");
  MatchBlock::Write(
      m, MatchBlock::State::Fresh(MatchBlock::MODE_RANKED, Ranked::RULESET_PPLUS_2024, coin));
  RankedPPlus::Apply(m, 1, true);
  SetScene(m, "scSelStage");
  return m;
}
}  // namespace

TEST(OrcaRankedSteps, PPlusGameOneIsOneTwoOneOnTheStartersFromTheCoin)
{
  const Ranked::Ruleset& r = Ranked::PPlus2024();
  for (int coin = 0; coin < 2; ++coin)
  {
    Ranked::SetView v;
    v.coin = coin;
    const Ranked::Plan plan = Ranked::PlanGame(r, v);
    ASSERT_EQ(plan.steps.size(), 3u);
    EXPECT_EQ(plan.available, r.starters);
    EXPECT_EQ(plan.steps[0], (Ranked::Step{StepKind::Strike, static_cast<u8>(coin), 1, false, 30}));
    EXPECT_EQ(plan.steps[1],
              (Ranked::Step{StepKind::Strike, static_cast<u8>(1 - coin), 2, false, 30}));
    EXPECT_EQ(plan.steps[2], (Ranked::Step{StepKind::Pick, static_cast<u8>(coin), 1, false, 10}));
  }
}

TEST(OrcaRankedSteps, PPlusLaterGamesWinnerStrikesTwoLoserPicksWithFullDsr)
{
  const Ranked::Ruleset& r = Ranked::PPlus2024();
  Ranked::SetView v;
  v.game = 3;
  v.last_winner = 1;
  // Port 1 won game 1 on Battlefield, port 2 won game 2 on Smashville; port 1 picks game 3.
  v.won_on = {0x0001, 0x0004};
  const Ranked::Plan plan = Ranked::PlanGame(r, v);
  ASSERT_EQ(plan.steps.size(), 2u);
  EXPECT_EQ(plan.steps[0], (Ranked::Step{StepKind::Strike, 1, 2, false, 30}));
  EXPECT_EQ(plan.steps[1], (Ranked::Step{StepKind::Pick, 0, 1, false, 30}));
  EXPECT_EQ(plan.available, r.All() & ~StageMask{0x0001});
  // The winner strikes Smashville and PS2; the loser may pick the 6 left, never Battlefield.
  EXPECT_EQ(Ranked::Allowed(plan, 1, 0x0006), r.All() & ~StageMask{0x0007});
}

TEST(OrcaRankedSteps, BrawlBanIsOptionalMetaKnightAndIceClimbersClauses)
{
  const Ranked::Ruleset& r = Ranked::Brawl2025();
  Ranked::SetView v;
  v.game = 2;
  v.last_winner = 0;
  Ranked::Plan plan = Ranked::PlanGame(r, v);
  ASSERT_EQ(plan.steps.size(), 2u);
  EXPECT_EQ(plan.steps[0], (Ranked::Step{StepKind::Ban, 0, 1, true, 30}));
  EXPECT_EQ(Ranked::DefaultStrikes(plan, 0, 0, 0), 0);  // the ban's timer: no ban
  // Ice Climbers: FD (5) never available.
  v.characters = {0x10, 0x00};
  plan = Ranked::PlanGame(r, v);
  EXPECT_EQ(plan.available & (1 << 5), 0);
  // Exactly one Meta Knight (port 1): port 2 picks any stage, game 1 too.
  v = {};
  v.characters = {0x18, 0x00};
  plan = Ranked::PlanGame(r, v);
  ASSERT_EQ(plan.steps.size(), 1u);
  EXPECT_EQ(plan.steps[0].kind, StepKind::Pick);
  EXPECT_EQ(plan.steps[0].port, 1);
  EXPECT_EQ(plan.available, r.All());
  // Both Meta Knight: no clause.
  v.characters = {0x18, 0x18};
  EXPECT_EQ(Ranked::PlanGame(r, v).steps.size(), 3u);
}

TEST(OrcaRankedSteps, TimerDefaultsTakeTheFirstStagesInListOrder)
{
  const Ranked::Ruleset& r = Ranked::PPlus2024();
  Ranked::SetView v;
  const Ranked::Plan plan = Ranked::PlanGame(r, v);
  EXPECT_EQ(Ranked::DefaultStrikes(plan, 0, 0, 0), 0x0001);
  EXPECT_EQ(Ranked::DefaultStrikes(plan, 1, 0x0001, 0), 0x0006);
  EXPECT_EQ(Ranked::DefaultStrikes(plan, 1, 0x0001, 1), 0x0002);
  EXPECT_EQ(Ranked::DefaultPick(plan, 2, 0x0007), 3);
  EXPECT_EQ(Ranked::StepFrames(r, plan.steps[0]), 30u * 60 + 180);
}

TEST(OrcaRankedSteps, CoinIsFairAndTheSameEverywhere)
{
  int ones = 0;
  for (int i = 0; i < 2000; ++i)
    ones += MatchBlock::CoinFromCode("ROOM" + std::to_string(i * 7919));
  EXPECT_GT(ones, 900);
  EXPECT_LT(ones, 1100);
  EXPECT_EQ(MatchBlock::CoinFromCode("K7Q2"), MatchBlock::CoinFromCode("K7Q2"));
}

TEST(OrcaMatchBlock, RoundTripsAndWritesOnlyWhatChanged)
{
  FakeMemory m = Game("scSelctCharacter");
  EXPECT_FALSE(MatchBlock::Read(m).has_value());  // the dead code is no header
  MatchBlock::State s = MatchBlock::State::Fresh(MatchBlock::MODE_RANKED, 2, 1);
  s.score = {1, 0};
  s.won_on = {0x0100, 0};
  s.deadline = 123456;
  EXPECT_GT(MatchBlock::Write(m, s), 0);
  EXPECT_EQ(MatchBlock::Read(m), s);
  EXPECT_EQ(MatchBlock::Write(m, s), 0);
  EXPECT_EQ(MatchBlock::Clear(m), 4);
  EXPECT_FALSE(MatchBlock::Read(m).has_value());
  EXPECT_EQ(MatchBlock::Clear(m), 0);
  EXPECT_EQ(m.Read32(MatchBlock::BASE), FreeSpace::kMatchBlockOriginalWord);
  // Written again after a clear; never over memory that isn't the dead code.
  EXPECT_GT(MatchBlock::Write(m, s), 0);
  MatchBlock::Clear(m);
  m.Write32(MatchBlock::BASE, 0x12345678);
  EXPECT_EQ(MatchBlock::Write(m, s), 0);
  EXPECT_FALSE(MatchBlock::Read(m).has_value());
}

TEST(OrcaRankedPPlus, PageZeroOfTheProposedPresetIsTheLegalList)
{
  FakeMemory m = Game("scSelctCharacter");
  for (u32 i = 0; i < RankedPPlus::RSS_EXDATA_SIZE; ++i)
    m.Write8(RankedPPlus::RSS_EXDATA + i, RankedPPlus::kSwitch03[i]);
  const std::vector<int> page = RankedPPlus::PageZero(m);
  ASSERT_EQ(page.size(), 9u);
  for (int p = 0; p < 9; ++p)
    EXPECT_EQ(page[p], POS_LEGAL[p]) << "position " << p;
}

TEST(OrcaRankedPPlus, NothingWithoutARankedBlock)
{
  FakeMemory m = Game("scSelStage");
  const auto before = m.bytes;
  EXPECT_EQ(RankedPPlus::Apply(m, 10, true), 0);
  EXPECT_EQ(m.bytes, before);
  const auto masks = RankedPPlus::Masks(m);
  for (const auto& mask : masks)
    EXPECT_TRUE(mask.Empty());
}

TEST(OrcaRankedPPlus, PresetOutsideTheStageSelectOnly)
{
  FakeMemory m = Game("scSelctCharacter");
  MatchBlock::Write(
      m, MatchBlock::State::Fresh(MatchBlock::MODE_RANKED, Ranked::RULESET_PPLUS_2024, 0));
  EXPECT_GT(RankedPPlus::Apply(m, 1, true), 0);
  for (u32 i = 0; i < RankedPPlus::RSS_EXDATA_SIZE; ++i)
    ASSERT_EQ(m.Read8(RankedPPlus::RSS_EXDATA + i), RankedPPlus::kSwitch03[i]);
  EXPECT_EQ(RankedPPlus::Apply(m, 2, true), 0);
  // On the stage select it's left alone.
  SetScene(m, "scSelStage");
  m.Write8(RankedPPlus::RSS_EXDATA, 0x77);
  RankedPPlus::Apply(m, 3, true);
  EXPECT_EQ(m.Read8(RankedPPlus::RSS_EXDATA), 0x77);
}

TEST(OrcaRankedPPlus, GameOneStrikesInTurnsThenTheCoinPicks)
{
  FakeMemory m = RankedSss(1);  // port 2 strikes first
  int frame = 100;
  EXPECT_GT(RankedPPlus::Apply(m, frame, true), 0);
  MatchBlock::State s = Block(m);
  EXPECT_EQ(s.flow, 1);
  EXPECT_EQ(s.step, 0);
  EXPECT_EQ(s.active, 1);
  EXPECT_EQ(m.Read32(TASK + RankedPPlus::SSS_CONTROLLER), 1u);
  // The counterpicks struck (not in play), the other pages all struck, a redraw asked for.
  EXPECT_EQ(Page0Bits(m), BitsOf(0x01E0));
  EXPECT_EQ(m.Read16(RankedPPlus::STAGE_STRIKE_TABLE + 6), 0xFFFF);
  EXPECT_EQ(m.Read8(RankedPPlus::PAGE_INDEX), 0xFF);
  // Idempotent: the hook again at the same boundary (after a rollback's load) changes nothing.
  EXPECT_EQ(RankedPPlus::Apply(m, frame, true), 0);

  // Masks: port 1 sees nothing, port 2 may move and press X only.
  auto masks = RankedPPlus::Masks(m);
  EXPECT_EQ(masks[0], Rollback::InputGate::ALL);
  EXPECT_FALSE(masks[1].main_stick);
  EXPECT_TRUE(masks[1].buttons & PAD_BUTTON_A);
  EXPECT_TRUE(masks[1].buttons & PAD_BUTTON_B);
  EXPECT_TRUE(masks[1].buttons & PAD_TRIGGER_L);
  EXPECT_TRUE(masks[1].buttons & PAD_TRIGGER_Z);
  EXPECT_FALSE(masks[1].buttons & PAD_BUTTON_X);
  EXPECT_EQ(masks[2], Rollback::InputGate::ALL);
  // On Random, X is masked too; on the page button it strikes the stage last hovered, as ever.
  m.Write32(TASK + RankedPPlus::SSS_HOVERED, RankedPPlus::RANDOM_ITEM);
  EXPECT_TRUE(RankedPPlus::Masks(m)[1].buttons & PAD_BUTTON_X);
  m.Write32(TASK + RankedPPlus::SSS_HOVERED, RankedPPlus::PAGE_ITEM);
  EXPECT_FALSE(RankedPPlus::Masks(m)[1].buttons & PAD_BUTTON_X);
  m.Write32(TASK + RankedPPlus::SSS_HOVERED, 0);
  EXPECT_TRUE(RankedPPlus::Masks(m)[1].c_stick);

  // Port 2 strikes Smashville: port 1's turn, two strikes.
  StrikeAt(m, 2);
  RankedPPlus::Apply(m, ++frame, true);
  s = Block(m);
  EXPECT_EQ(s.struck, 0x0004);
  EXPECT_EQ(s.step, 1);
  EXPECT_EQ(s.active, 0);
  EXPECT_EQ(m.Read32(TASK + RankedPPlus::SSS_CONTROLLER), 0u);
  // A strike on a counterpick (already struck) counts for nothing.
  StrikeAt(m, 6);
  RankedPPlus::Apply(m, ++frame, true);
  EXPECT_EQ(Block(m).step_done, 0);
  StrikeAt(m, 0);
  RankedPPlus::Apply(m, ++frame, true);
  EXPECT_EQ(Block(m).step, 1);
  EXPECT_EQ(Block(m).step_done, 1);
  StrikeAt(m, 4);
  RankedPPlus::Apply(m, ++frame, true);
  s = Block(m);
  EXPECT_EQ(s.struck, 0x0015);
  EXPECT_EQ(s.step, 2);
  EXPECT_EQ(s.active, 1);
  // The pick: A allowed, X masked; not A on Random or the items past it, nor on the stage
  // builder's page except on the page button.
  masks = RankedPPlus::Masks(m);
  EXPECT_FALSE(masks[1].buttons & PAD_BUTTON_A);
  EXPECT_TRUE(masks[1].buttons & PAD_BUTTON_X);
  m.Write32(TASK + RankedPPlus::SSS_HOVERED, 0x37);
  EXPECT_TRUE(RankedPPlus::Masks(m)[1].buttons & PAD_BUTTON_A);
  m.Write32(TASK + RankedPPlus::SSS_PAGE, RankedPPlus::CUSTOM_PAGE);
  m.Write32(TASK + RankedPPlus::SSS_HOVERED, 3);
  EXPECT_TRUE(RankedPPlus::Masks(m)[1].buttons & PAD_BUTTON_A);
  m.Write32(TASK + RankedPPlus::SSS_HOVERED, RankedPPlus::PAGE_ITEM);
  EXPECT_FALSE(RankedPPlus::Masks(m)[1].buttons & PAD_BUTTON_A);
  m.Write32(TASK + RankedPPlus::SSS_PAGE, 0);
  m.Write32(TASK + RankedPPlus::SSS_HOVERED, 0);

  // Port 2 picks PS2: the fight starts on it, and port 2 wins it.
  m.Write8(MODE_MELEE + 0x1B, 0x2E);
  SetScene(m, "scMemoryChange");
  RankedPPlus::Apply(m, ++frame, true);
  EXPECT_EQ(Block(m).flow, 1);
  SetScene(m, "scMelee");
  RankedPPlus::Apply(m, ++frame, true);
  s = Block(m);
  EXPECT_TRUE(s.fight);
  EXPECT_EQ(s.flow, 0);
  EXPECT_EQ(s.stage, 1);
  EXPECT_TRUE(RankedPPlus::Masks(m)[0].Empty());
  SetResult(m, 1);
  SetScene(m, "scVsResult");
  RankedPPlus::Apply(m, ++frame, true);
  s = Block(m);
  EXPECT_FALSE(s.fight);
  EXPECT_EQ(s.games, 1);
  EXPECT_EQ(s.score[1], 1);
  EXPECT_EQ(s.last_winner, 1);
  EXPECT_EQ(s.won_on[1], 0x0002);
  EXPECT_EQ(s.records[0].winner, 1);
  EXPECT_EQ(s.records[0].stage, 1);
  // Counted once.
  RankedPPlus::Apply(m, ++frame, true);
  EXPECT_EQ(Block(m).games, 1);

  // Game 2: the stage select again; port 2 (the winner) strikes 2 from all but PS2 (port 1 never
  // won); then port 1 picks.
  SetScene(m, "scSelctCharacter");
  RankedPPlus::Apply(m, ++frame, true);
  SetScene(m, "scSelStage");
  m.Fill(RankedPPlus::STAGE_STRIKE_TABLE - 1, 0x1F, 0);  // the task's create clears it
  RankedPPlus::Apply(m, ++frame, true);
  s = Block(m);
  EXPECT_EQ(s.active, 1);
  EXPECT_EQ(s.struck, 0);
  EXPECT_EQ(Page0Bits(m), 0u);
  StrikeAt(m, 7);
  StrikeAt(m, 8);
  RankedPPlus::Apply(m, ++frame, true);
  s = Block(m);
  EXPECT_EQ(s.struck, 0x0180);
  EXPECT_EQ(s.step, 1);
  EXPECT_EQ(s.active, 0);
}

TEST(OrcaRankedPPlus, TimersStrikeTheFirstStagesThenPickForThePlayer)
{
  FakeMemory m = RankedSss(0);
  int frame = 1000;
  RankedPPlus::Apply(m, frame, true);
  // Port 1's 30 s (and the grace) run out: Battlefield is struck for it.
  frame += 30 * 60 + 180;
  RankedPPlus::Apply(m, frame, true);
  MatchBlock::State s = Block(m);
  EXPECT_EQ(s.struck, 0x0001);
  EXPECT_EQ(s.step, 1);
  EXPECT_EQ(s.deadline, static_cast<u32>(frame + 30 * 60 + 180));
  // The struck art for the default strike: a redraw.
  EXPECT_EQ(Page0Bits(m), BitsOf(0x01E1));
  frame += 30 * 60 + 180;
  RankedPPlus::Apply(m, frame, true);
  s = Block(m);
  EXPECT_EQ(s.struck, 0x0007);
  EXPECT_EQ(s.step, 2);
  // The pick's 10 s run out: Luigi's Mansion, the first left, picked for port 1.
  frame += 10 * 60 + 180;
  RankedPPlus::Apply(m, frame, true);
  s = Block(m);
  EXPECT_TRUE(s.auto_pick);
  EXPECT_EQ(s.auto_stage, 3);
  EXPECT_EQ(m.Read32(TASK + RankedPPlus::SSS_SELECTED), static_cast<u32>(PosOf(3)));
  EXPECT_EQ(Page0Bits(m), BitsOf(0x01FF & ~0x0008));
  const auto masks = RankedPPlus::Masks(m);
  EXPECT_EQ(masks[0].press, PAD_BUTTON_A);
  EXPECT_TRUE(masks[0].main_stick);
  EXPECT_EQ(masks[1], Rollback::InputGate::ALL);
  // Idempotent at that boundary too.
  EXPECT_EQ(RankedPPlus::Apply(m, frame, true), 0);
}

TEST(OrcaRankedPPlus, TwoWinsTakeTheSetAndFreeTheStageSelect)
{
  FakeMemory m = RankedSss(0);
  MatchBlock::State s = Block(m);
  s.games = 1;
  s.score = {1, 0};
  s.last_winner = 0;
  s.fight = true;
  s.stage = 0;
  MatchBlock::Write(m, s);
  SetResult(m, 0);
  SetScene(m, "scVsResult");
  RankedPPlus::Apply(m, 50, true);
  s = Block(m);
  EXPECT_TRUE(s.done);
  EXPECT_EQ(s.set_winner, 0);
  EXPECT_EQ(s.score[0], 2);
  SetScene(m, "scSelStage");
  RankedPPlus::Apply(m, 51, true);
  EXPECT_EQ(Block(m).flow, 0);
  for (const auto& mask : RankedPPlus::Masks(m))
    EXPECT_TRUE(mask.Empty());
}

TEST(OrcaRankedPPlus, AnOpponentWhoLeavesFreesTheStageSelect)
{
  FakeMemory m = RankedSss(0);
  RankedPPlus::Apply(m, 10, true);
  EXPECT_EQ(Block(m).flow, 1);
  RankedPPlus::Apply(m, 11, false);
  EXPECT_EQ(Block(m).flow, 0);
  for (const auto& mask : RankedPPlus::Masks(m))
    EXPECT_TRUE(mask.Empty());
}

TEST(OrcaRankedPPlus, OverlayLines)
{
  FakeMemory m = RankedSss(1);
  RankedPPlus::Apply(m, 100, true);
  const std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, {}}, {1, "bo", true, {}}};
  RankedPPlus::Lines l = RankedPPlus::Describe(m, 100, ports);
  EXPECT_EQ(l.set, "Ranked · Game 1 · ada 0–0 bo");
  EXPECT_EQ(l.turn, "bo strikes 1 stage · 0:30");
  const std::vector<Orca::Events::PortInfo> theirs{{0, "ada", true, {}}, {1, "bo", false, {}}};
  l = RankedPPlus::Describe(m, 100 + 6 * 60, theirs);
  EXPECT_EQ(l.turn, "Your turn: strike 1 stage with X · 0:24");
  SetScene(m, "scMelee");
  EXPECT_EQ(RankedPPlus::Describe(m, 100, ports), RankedPPlus::Lines{});
}
