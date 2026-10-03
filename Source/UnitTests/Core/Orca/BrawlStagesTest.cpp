// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// A ranked set's stage flow on vanilla Brawl's stage select (BrawlStages.h), through a fake of the
// game's memory: the steps follow the game's pads and cursor, every write is idempotent at its
// boundary, the timer counts the screen's own frames, the gate keeps the off-turn port out.

#include <map>
#include <string_view>

#include <gtest/gtest.h>

#include "Core/Orca/UX/BrawlStages.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Rollback/InputGate.h"
#include "InputCommon/GCPadStatus.h"

using namespace Orca::UX;
using namespace Orca::UX::BrawlStages;
namespace RS = Orca::UX::RankedSteps;

namespace
{
class FakeMemory final : public GuestMemory
{
public:
  bool Valid(u32 a) const override { return bytes.contains(a); }
  u8 Read8(u32 a) const override { return bytes.at(a); }
  u16 Read16(u32 a) const override { return static_cast<u16>(Read8(a) << 8 | Read8(a + 1)); }
  u32 Read32(u32 a) const override { return u32(Read16(a)) << 16 | Read16(a + 2); }
  void Write8(u32 a, u8 v) override
  {
    bytes[a] = v;
    ++writes;
  }
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
  int writes = 0;
};

constexpr u32 MANAGER = 0x80900000;
constexpr u32 SCENE = 0x80910000;
constexpr u32 NAME = 0x80920000;
constexpr u32 TASK = 0x80930000;
constexpr u32 ICONS = 0x80940000;
constexpr u32 CURSOR = 0x80948000;
constexpr u32 PADS = 0x80950000;
constexpr u32 GLOBAL = 0x90181300;
constexpr u32 MELEE = 0x90180F20;
constexpr u32 RESULT = 0x9017F420;
constexpr int SLOTS = 40;

constexpr int MARIO = 0;
constexpr int LINK = 2;

// Brawl in a scene; the stage select's task with its icons on `page`; two human players.
FakeMemory Game(std::string_view scene, u32 page = 0)
{
  FakeMemory m;
  m.Fill(0x805A0040, 0xC0);
  m.Write32(0x805A0060, MANAGER);
  m.Write32(0x805A0040, PADS);
  m.Write32(0x805A00BC, 0x12345678);  // the RNG's seed
  m.Write32(0x805A00E0, GLOBAL);
  m.Fill(MANAGER, 0x20);
  m.Write32(MANAGER + 4, SCENE);
  m.Fill(SCENE, 0x400);
  m.Write32(SCENE, NAME);
  m.Write32(SCENE + 0x3AC, TASK);
  m.Text(NAME, scene);
  m.Fill(TASK, 0x300);
  m.Write32(TASK + 0x200, CURSOR);
  m.Write32(TASK + 0x228, page);
  m.Fill(0x806B9298, 0x18);
  m.Write8(0x806B9298 + 4, 31);
  m.Write8(0x806B9298 + 8 + 4, 10);
  m.Write32(TASK + 0x244, 0);
  m.Write32(TASK + 0x248, 0xFFFFFFFF);
  m.Write32(TASK + 0x278, 0xF0);
  m.Fill(CURSOR, 0x50);
  for (u32 i = 0; i < SLOTS; ++i)
  {
    const u32 icon = ICONS + i * 0x100;
    m.Fill(icon, 0x100);
    m.Write32(TASK + 0x8C + 4 * i, icon);
    m.Write32(icon + 0x14, icon + 0x40);
    m.Write32(icon + 0x40 + 0x10, icon + 0x80);
    m.Write32(icon + 0x80 + 0x18, 0x3F800000 + i);  // its picture
  }
  m.Fill(PADS, 0x200);
  m.Fill(GLOBAL, 0x40);
  m.Write32(GLOBAL + 0x08, MELEE);
  m.Write32(GLOBAL + 0x18, RESULT);
  m.Fill(MELEE, 0x98 + 4 * 0x5C);
  for (u32 port = 0; port < 4; ++port)
    m.Write8(MELEE + 0x98 + port * 0x5C + 1, port < 2 ? 0 : 3);
  m.Write8(MELEE + 0x98, MARIO);
  m.Write8(MELEE + 0x98 + 0x5C, LINK);
  m.Fill(RESULT, 0x24 + 4 * 0x2AC + 4);
  m.Fill(kRegion, kRegionSize, 0x60);  // the dead code that's there before Orca writes it
  return m;
}

void Scene(FakeMemory& m, std::string_view scene)
{
  m.Text(NAME, scene);
}

void Hover(FakeMemory& m, int stage)
{
  const Tile& t = TileFor(stage);
  m.Write32(TASK + 0x228, t.page);
  m.Write32(TASK + 0x244, t.slot + 2u);  // the hovered item (page 0: the slot + 2)
  m.Write32(TASK + 0x248, t.slot);
}

void Pad(FakeMemory& m, int port, u16 buttons)
{
  m.Write32(PADS + 0x40 + 0x40 * static_cast<u32>(port), buttons);
}

// One frame of the stage select: its clock ticks, then the boundary runs (twice when `again`, as
// after a rollback's load: the second run must change nothing).
void Boundary(FakeMemory& m, bool again = true)
{
  m.Write32(TASK + 0x250, m.Read32(TASK + 0x250) + 1);
  Frame(m);
  if (again)
  {
    const auto before = m.bytes;
    Frame(m);
    EXPECT_EQ(before, m.bytes) << "a boundary run again changed memory";
  }
}

void Press(FakeMemory& m, int port, u16 button)
{
  Pad(m, port, button);
  Boundary(m);
  Pad(m, port, 0);
  Boundary(m);
}

u32 IconFrame(const FakeMemory& m, int slot)
{
  return m.Read32(ICONS + static_cast<u32>(slot) * 0x100 + 0x80 + 0x18);
}

class OrcaBrawlStages : public ::testing::Test
{
protected:
  void SetUp() override { SetRankedForTests(true); }
  void TearDown() override { SetRankedForTests(std::nullopt); }
};
}  // namespace

TEST_F(OrcaBrawlStages, NothingWithoutARankedSet)
{
  SetRankedForTests(false);
  FakeMemory m = Game("scSelStage");
  const auto before = m.bytes;
  Boundary(m);
  m.Write32(TASK + 0x250, 0);
  EXPECT_EQ(before, m.bytes);
  Rollback::InputGate::Masks masks{};
  Masks(m, &masks);
  for (const auto& mask : masks)
    EXPECT_TRUE(mask.Empty());
}

TEST_F(OrcaBrawlStages, GameOneStrikesOnTheStageSelect)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  auto s = ReadState(m);
  ASSERT_TRUE(s);
  EXPECT_EQ(s->phase, Phase::Select);
  EXPECT_EQ(s->set.game, 1);
  ASSERT_NE(s->coin, 0xFF);
  EXPECT_EQ(s->coin, RS::CoinFromSeed(0x12345678));
  EXPECT_EQ(s->characters[0], MARIO);
  EXPECT_EQ(s->characters[1], LINK);
  const int first = s->coin;
  const int second = 1 - first;
  // The striker's port drives the cursor.
  EXPECT_EQ(m.Read32(TASK + 0x278), static_cast<u32>(first));

  // The other port's X does nothing; the striker's X on a starter strikes it.
  Hover(m, RS::Battlefield);
  Press(m, second, BrawlStages::GF_X);
  EXPECT_EQ(ReadState(m)->progress.struck, 0);
  Press(m, first, BrawlStages::GF_X);
  s = ReadState(m);
  EXPECT_EQ(s->progress.struck, 1 << RS::Battlefield);
  EXPECT_EQ(m.Read32(TASK + 0x278), static_cast<u32>(second));
  // A held X is one strike.
  Hover(m, RS::Smashville);
  Pad(m, second, BrawlStages::GF_X);
  Boundary(m);
  Hover(m, RS::LylatCruise);
  Boundary(m);
  EXPECT_EQ(ReadState(m)->progress.struck, (1 << RS::Battlefield) | (1 << RS::Smashville));
  Pad(m, second, 0);
  Boundary(m);
  // A counterpick can't be struck in game 1.
  Hover(m, RS::FinalDestination);
  Press(m, second, BrawlStages::GF_X);
  EXPECT_EQ(ReadState(m)->progress.struck, (1 << RS::Battlefield) | (1 << RS::Smashville));
  Hover(m, RS::LylatCruise);
  Press(m, second, BrawlStages::GF_X);
  Hover(m, RS::YoshisIsland);
  Press(m, first, BrawlStages::GF_X);
  s = ReadState(m);
  EXPECT_EQ(s->progress.picked, RS::PokemonStadium);
  // The struck stages' icons are grey on their page.
  m.Write32(TASK + 0x228, TileFor(RS::Battlefield).page);
  Boundary(m);
  EXPECT_EQ(IconFrame(m, TileFor(RS::Battlefield).slot), 0u);
}

TEST_F(OrcaBrawlStages, TheTimerCountsTheScreensFrames)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  const u16 start = ReadState(m)->progress.frames;
  for (int i = 0; i < 10; ++i)
    Boundary(m);
  EXPECT_EQ(ReadState(m)->progress.frames, start - 10);
  // No tick without a frame of the screen.
  Frame(m);
  EXPECT_EQ(ReadState(m)->progress.frames, start - 10);
  // Run out: the first starter is struck by default.
  for (int i = 0; i < start; ++i)
    Boundary(m, false);
  EXPECT_EQ(ReadState(m)->progress.struck, 1 << RS::Battlefield);
}

TEST_F(OrcaBrawlStages, TheGateKeepsTheOffTurnPortOut)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  const int first = ReadState(m)->coin;
  Rollback::InputGate::Masks masks{};
  Masks(m, &masks);
  EXPECT_EQ(masks[1 - first], Rollback::InputGate::ALL);
  EXPECT_EQ(masks[2], Rollback::InputGate::ALL);
  // The striker: no B, Start, Z, L, R, and no A while striking (except on the page button).
  const auto& mine = masks[first];
  EXPECT_TRUE(mine.buttons & PAD_BUTTON_B);
  EXPECT_TRUE(mine.buttons & PAD_BUTTON_START);
  EXPECT_TRUE(mine.buttons & PAD_BUTTON_A);
  EXPECT_FALSE(mine.buttons & PAD_BUTTON_X);
  EXPECT_FALSE(mine.main_stick);
  m.Write32(TASK + 0x244, 0x35);  // the page button
  masks = {};
  Masks(m, &masks);
  EXPECT_FALSE(masks[first].buttons & PAD_BUTTON_A);
}

TEST_F(OrcaBrawlStages, ResultsMakeTheSetAndLaterGamesBanThenPick)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  Scene(m, "scMelee");
  Boundary(m);
  EXPECT_EQ(ReadState(m)->phase, Phase::Fight);
  // Port 2 wins on Smashville.
  Scene(m, "scVsResult");
  m.Write16(RESULT + 0x0C, 0x21);
  m.Write16(RESULT + 0x0E, 1);
  for (u32 port = 0; port < 4; ++port)
  {
    const u32 rec = RESULT + 0x24 + port * 0x2AC;
    m.Write8(rec + 1, port < 2 ? 0 : 3);
    m.Write8(rec + 0x0E, port == 1 ? 0 : 1);
  }
  Boundary(m);
  Boundary(m);
  auto s = ReadState(m);
  EXPECT_EQ(s->set.game, 2);
  EXPECT_EQ(s->score[0], 0);
  EXPECT_EQ(s->score[1], 1);
  EXPECT_EQ(s->set.last_winner, 1);
  EXPECT_EQ(s->set.won[1], 1 << RS::Smashville);
  // Game 2: port 2 may ban, then port 1 picks.
  Scene(m, "scSelctCharacter");
  Boundary(m);
  Scene(m, "scSelStage");
  Boundary(m);
  s = ReadState(m);
  EXPECT_EQ(s->phase, Phase::Select);
  EXPECT_EQ(m.Read32(TASK + 0x278), 1u);
  Press(m, 1, BrawlStages::GF_Y);  // no ban
  EXPECT_EQ(m.Read32(TASK + 0x278), 0u);
  // A picks only an allowed stage the cursor rested on.
  Hover(m, RS::DelfinoPlaza);
  Boundary(m);
  Boundary(m);
  Rollback::InputGate::Masks masks{};
  Masks(m, &masks);
  EXPECT_FALSE(masks[0].buttons & PAD_BUTTON_A);
}

TEST_F(OrcaBrawlStages, AFriendsGameClearsTheSet)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  ASSERT_TRUE(ReadState(m));
  SetRankedForTests(false);
  Boundary(m);
  EXPECT_FALSE(ReadState(m));
}
