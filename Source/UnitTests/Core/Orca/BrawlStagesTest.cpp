// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// A ranked set's stage flow on vanilla Brawl's stage select (BrawlStages.h), through a fake of the
// game's memory: the steps follow the game's pads and cursor, every write is idempotent at its
// boundary, the timer counts the screen's own frames, the gate keeps the off-turn port out.

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/UX/BrawlStages.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Rollback/InputGate.h"
#include "InputCommon/GCPadStatus.h"

using namespace Orca::UX;
using namespace Orca::UX::BrawlStages;
namespace RS = Orca::UX::Ranked;

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
  m.Fill(kBlock, 0x10);
  m.Write8(kBlock + 7, 1);  // the header's coin: port 2 strikes first
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
  EXPECT_EQ(s->coin, 1);  // the header's (the room code's)
  EXPECT_EQ(s->characters[0], MARIO);
  EXPECT_EQ(s->characters[1], LINK);
  const int first = s->coin;
  const int second = 1 - first;
  // The striker's port drives the cursor.
  EXPECT_EQ(m.Read32(TASK + 0x278), static_cast<u32>(first));

  // The other port's X does nothing; the striker's X on a starter strikes it.
  Hover(m, Battlefield);
  Press(m, second, BrawlStages::GF_X);
  EXPECT_EQ(ReadState(m)->progress.struck, 0);
  Press(m, first, BrawlStages::GF_X);
  s = ReadState(m);
  EXPECT_EQ(s->progress.struck, 1 << Battlefield);
  EXPECT_EQ(m.Read32(TASK + 0x278), static_cast<u32>(second));
  // A held X is one strike.
  Hover(m, Smashville);
  Pad(m, second, BrawlStages::GF_X);
  Boundary(m);
  Hover(m, LylatCruise);
  Boundary(m);
  EXPECT_EQ(ReadState(m)->progress.struck, (1 << Battlefield) | (1 << Smashville));
  Pad(m, second, 0);
  Boundary(m);
  // A counterpick can't be struck in game 1.
  Hover(m, FinalDestination);
  Press(m, second, BrawlStages::GF_X);
  EXPECT_EQ(ReadState(m)->progress.struck, (1 << Battlefield) | (1 << Smashville));
  Hover(m, LylatCruise);
  Press(m, second, BrawlStages::GF_X);
  // The first striker picks from the last two (X strikes nothing more).
  Hover(m, YoshisIsland);
  Press(m, first, BrawlStages::GF_X);
  s = ReadState(m);
  EXPECT_EQ(s->progress.struck,
            (1 << Battlefield) | (1 << Smashville) | (1 << LylatCruise));
  EXPECT_EQ(s->progress.picked, -1);
  EXPECT_EQ(m.Read32(TASK + 0x278), static_cast<u32>(first));
  // The struck stages' icons are grey on their page.
  m.Write32(TASK + 0x228, TileFor(Battlefield).page);
  Boundary(m);
  EXPECT_EQ(IconFrame(m, TileFor(Battlefield).slot), 0u);
  // The game takes Pokemon Stadium for the picker.
  m.Write32(TASK + 0x224, 2);
  m.Write32(TASK + 0x258, 0x2E);
  Boundary(m);
  EXPECT_EQ(ReadState(m)->progress.picked, PokemonStadium);
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
  EXPECT_EQ(ReadState(m)->progress.struck, 1 << Battlefield);
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
  // A turns the page once the cursor has rested on the page button a frame.
  m.Write32(TASK + 0x244, 0x35);
  Boundary(m);
  masks = {};
  Masks(m, &masks);
  EXPECT_TRUE(masks[first].buttons & PAD_BUTTON_A);
  Boundary(m);
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
  EXPECT_EQ(s->set.won_on[1], 1 << Smashville);
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
  Hover(m, DelfinoPlaza);
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

TEST_F(OrcaBrawlStages, ATakenStageTheStepsDontAllowIsReplaced)
{
  // Game 2: port 2 won on Smashville and skips its ban; port 1 picks.
  FakeMemory m = Game("scSelStage");
  State s;
  s.set.game = 2;
  s.set.last_winner = 1;
  s.set.won_on = {1 << FinalDestination, 1 << Smashville};
  s.score = {0, 1};
  s.coin = 0;
  s.phase = Phase::Recorded;
  WriteState(m, s);
  Boundary(m);
  Press(m, 1, BrawlStages::GF_Y);
  RS::SetView view = ReadState(m)->set;
  view.characters = {0, 2};
  const RS::Plan plan = RS::PlanGame(RS::Brawl2025(), view);
  ASSERT_NE(RS::Current(plan, ReadState(m)->progress), nullptr);
  ASSERT_EQ(RS::Current(plan, ReadState(m)->progress)->kind, RS::StepKind::Pick);
  // The cursor rests on Delfino, then A lands on Final Destination (port 1 won there: DSR).
  Hover(m, DelfinoPlaza);
  Boundary(m);
  Boundary(m);
  m.Write32(TASK + 0x224, 2);
  m.Write32(TASK + 0x258, 0x02);
  Boundary(m);
  EXPECT_EQ(m.Read32(TASK + 0x258), 0x03u);  // Delfino Plaza
  EXPECT_EQ(ReadState(m)->progress.picked, DelfinoPlaza);
  // Everybody's pads are off until the screen leaves.
  Rollback::InputGate::Masks masks{};
  Masks(m, &masks);
  EXPECT_EQ(masks[0], Rollback::InputGate::ALL);
  EXPECT_EQ(masks[1], Rollback::InputGate::ALL);
}

TEST_F(OrcaBrawlStages, AStageTakenDuringStrikesTakesTheDefaults)
{
  FakeMemory m = Game("scSelStage");
  Boundary(m);
  m.Write32(TASK + 0x224, 2);
  m.Write32(TASK + 0x258, 0x01);  // Battlefield
  Boundary(m);
  // Strikes by default: Battlefield, then Pokemon Stadium and Lylat, then the pick's default, the
  // first of the last two (Smashville).
  EXPECT_EQ(ReadState(m)->progress.picked, Smashville);
  EXPECT_EQ(m.Read32(TASK + 0x258), 0x21u);
}

TEST_F(OrcaBrawlStages, TheMatchBlocksHeaderTurnsTheFlowOn)
{
  SetRankedForTests(std::nullopt);
  FakeMemory m = Game("scSelStage");
  m.Fill(kBlock, 0x300, 0x60);
  Boundary(m);
  EXPECT_FALSE(ReadState(m));  // no header: friends or solo
  m.Write32(kBlock, kHeaderMagic);
  m.Write8(kBlock + 4, kHeaderVersion);
  m.Write8(kBlock + 5, kModeRanked);
  m.Write8(kBlock + 6, 2);  // Project+'s ruleset
  Boundary(m);
  EXPECT_FALSE(ReadState(m));
  m.Write8(kBlock + 5, kModeCasual);
  m.Write8(kBlock + 6, kRulesetBrawl);
  // Casual: its own stage pick, once both players are in.
  Frame(m, false);
  EXPECT_FALSE(ReadState(m));
  Boundary(m);
  ASSERT_TRUE(ReadState(m));
  EXPECT_NE(ReadState(m)->seed, 0u);
  m.Write8(kBlock + 5, kModeRanked);
  m.Write32(TASK + 0x250, 0);  // a new stage select
  Boundary(m);
  ASSERT_TRUE(ReadState(m));
  EXPECT_EQ(ReadState(m)->phase, Phase::Select);
  EXPECT_EQ(ReadState(m)->seed, 0u);
  // The header goes (the opponent left a casual game, the host is alone again): so does the flow.
  m.Write32(kBlock, 0);
  Boundary(m);
  EXPECT_FALSE(ReadState(m));
}

TEST_F(OrcaBrawlStages, TheResultsTracksSetDrivesTheSteps)
{
  FakeMemory m = Game("scSelStage");
  m.Fill(kBlock, 0x300, 0x60);
  m.Write32(kBlock, kHeaderMagic);
  m.Write8(kBlock + 4, kHeaderVersion);
  m.Write8(kBlock + 5, kModeRanked);
  m.Write8(kBlock + 6, kRulesetBrawl);
  // One game recorded: port 2 won on Smashville.
  m.Fill(kResultsSet, 0x80);
  m.Write8(kResultsSet + 0, 1);
  m.Write8(kResultsSet + 2, 1);
  m.Write8(kResultsSet + 4, 0xFF);
  m.Write8(kResultsSet + 5, 1);
  m.Write8(kResultsSet + 0x20 + 4, 1);
  m.Write8(kResultsSet + 0x20 + 6, 0x21);
  Boundary(m);
  std::array<u8, 2> score{};
  const RS::SetView set = SetOf(m, *ReadState(m), &score);
  EXPECT_EQ(set.game, 2);
  EXPECT_EQ(set.last_winner, 1);
  EXPECT_EQ(set.won_on[1], 1 << Smashville);
  EXPECT_EQ(score[1], 1);
  // Port 2 won: its ban comes first.
  EXPECT_EQ(m.Read32(TASK + 0x278), 1u);
  // A block whose set bytes aren't a set (the results track isn't there): this flow's own record.
  m.Write8(kResultsSet + 0, 0x60);
  EXPECT_EQ(SetOf(m, *ReadState(m)).game, 1);
}

TEST_F(OrcaBrawlStages, ANewStageSelectStartsTheStepsAgain)
{
  FakeMemory m = Game("scSelStage");
  for (int i = 0; i < 50; ++i)
    Boundary(m);
  Hover(m, Battlefield);
  Press(m, ReadState(m)->coin, BrawlStages::GF_X);
  ASSERT_NE(ReadState(m)->progress.struck, 0);
  // The set ended mid-select and another began on a new stage select: its frame counter is back
  // at the start.
  m.Write32(TASK + 0x250, 0);
  Boundary(m);
  EXPECT_EQ(ReadState(m)->progress.struck, 0);
  EXPECT_EQ(ReadState(m)->progress.step, 0);
}

TEST_F(OrcaBrawlStages, TheOverlaysTopLineHasTheSetTheTurnTheCountAndTheTime)
{
  FakeMemory m = Game("scSelStage");  // the header's coin: port 2 strikes first
  std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, {}}, {1, "bo", true, {}}};
  Scene(m, "scSelctCharacter");
  Boundary(m);
  EXPECT_EQ(Lines(m, ports), (std::pair<std::string, std::string>{"Game 1 · You 0–0 bo", ""}));
  Scene(m, "scSelStage");
  Boundary(m);
  EXPECT_EQ(Lines(m, ports),
            (std::pair<std::string, std::string>{"Game 1 · You 0–0 bo · bo strikes 1 · 0:30", ""}));
  Hover(m, Battlefield);
  Press(m, 1, BrawlStages::GF_X);
  // Port 1's two strikes, its keys under the line.
  EXPECT_EQ(Lines(m, ports), (std::pair<std::string, std::string>{
                                 "Game 1 · You 0–0 bo · You strike 2 · 0:30",
                                 "X on a stage strikes it"}));
  Hover(m, Smashville);
  Press(m, 0, BrawlStages::GF_X);
  EXPECT_EQ(Lines(m, ports).first, "Game 1 · You 0–0 bo · You strike 1 · 0:30");
}

namespace
{
class OrcaBrawlCasual : public ::testing::Test
{
protected:
  void SetUp() override
  {
    SetRankedForTests(false);
    SetCasualForTests(true);
  }
  void TearDown() override
  {
    SetRankedForTests(std::nullopt);
    SetCasualForTests(std::nullopt);
  }
};

constexpr u32 ROOM = 0x5EED1234;

RS::Plan CasualPlanOf(const State& s)
{
  RS::SetView v;
  v.coin = s.coin;
  v.characters = {s.characters[0], s.characters[1]};
  return RS::PlanCasual(RS::Brawl2025(), v, s.seed);
}
}  // namespace

TEST_F(OrcaBrawlCasual, EachPlayerNamesAStageThenTheCoinTakesOne)
{
  FakeMemory m = Game("scSelStage");  // the header's coin: port 2 names first
  m.Write32(kBlock + 8, ROOM);
  Boundary(m);
  auto s = ReadState(m);
  ASSERT_TRUE(s);
  EXPECT_EQ(s->phase, Phase::Select);
  EXPECT_EQ(s->coin, 1);
  EXPECT_EQ(s->set.game, 1);
  EXPECT_EQ(s->seed, RS::CasualSeed(ROOM, 1, 1));
  EXPECT_EQ(s->progress.frames, 7 * 60 + 30);
  EXPECT_EQ(m.Read32(TASK + 0x278), 1u);
  // Port 1 does nothing. Port 2: no B, Start, Z, L, R, X or Y; its A reads as X.
  Rollback::InputGate::Masks masks{};
  Masks(m, &masks);
  EXPECT_EQ(masks[0], Rollback::InputGate::ALL);
  for (const u16 off : {PAD_BUTTON_A, PAD_BUTTON_B, PAD_BUTTON_X, PAD_BUTTON_Y, PAD_BUTTON_START,
                        PAD_TRIGGER_Z, PAD_TRIGGER_L, PAD_TRIGGER_R})
  {
    EXPECT_TRUE(masks[1].buttons & off) << off;
  }
  EXPECT_EQ(masks[1].a_as, PAD_BUTTON_X);
  EXPECT_FALSE(masks[1].main_stick);
  // What the game reads from port 2's A: X, which vanilla's stage select ignores.
  GCPadStatus pad{};
  pad.isConnected = true;
  pad.button = PAD_BUTTON_A;
  EXPECT_EQ(Rollback::InputGate::Apply(pad, masks[1]).button, PAD_BUTTON_X);
  std::vector<Orca::Events::PortInfo> ports{{0, "ada", false, {}}, {1, "bo", true, {}}};
  EXPECT_EQ(Lines(m, ports),
            (std::pair<std::string, std::string>{"Stage pick · bo · 0:07", ""}));

  // Port 1's press does nothing; port 2's on Smashville names it, and it's port 1's turn.
  Hover(m, Smashville);
  Press(m, 0, BrawlStages::GF_X);
  EXPECT_EQ(ReadState(m)->progress.prefs[1], -1);
  Press(m, 1, BrawlStages::GF_X);
  s = ReadState(m);
  EXPECT_EQ(s->progress.prefs[1], Smashville);
  EXPECT_EQ(s->progress.step, 1);
  EXPECT_EQ(s->progress.picked, -1);
  EXPECT_EQ(m.Read32(TASK + 0x278), 0u);
  EXPECT_EQ(Lines(m, ports), (std::pair<std::string, std::string>{
                                 "Stage pick · your turn · 0:07",
                                 "A on the stage you want · then one of the two picks, at random"}));
  // On the page button (rested a frame) A turns the page, no X.
  m.Write32(TASK + 0x244, 0x35);
  Boundary(m);
  Boundary(m);
  masks = {};
  Masks(m, &masks);
  EXPECT_FALSE(masks[0].buttons & PAD_BUTTON_A);
  EXPECT_EQ(masks[0].a_as, 0);
  Hover(m, Battlefield);

  // Port 1's 7 s run out: a random legal stage for it, then the coin's pick.
  for (int i = 0; i < 7 * 60 + 30 && ReadState(m)->progress.picked < 0; ++i)
    Boundary(m, false);
  s = ReadState(m);
  const RS::Plan plan = CasualPlanOf(*s);
  EXPECT_EQ(s->progress.prefs[0], RS::CasualDefault(plan, 0));
  const int chosen = RS::CasualChoice(plan);
  ASSERT_GE(s->progress.picked, 0);
  EXPECT_EQ(s->progress.picked, s->progress.prefs[chosen]);
  const std::string whose = s->progress.prefs[0] == s->progress.prefs[1] ? "both picks" :
                            chosen == 0                                  ? "your pick" :
                                                                           "bo's pick";
  EXPECT_NE(Lines(m, ports).first.find("(" + whose + ")"), std::string::npos);
  // The flow takes it for the last turn's port: everyone's pad off, the cursor to the stage (or
  // the page button first), A pressed on it every other frame.
  Boundary(m);
  masks = {};
  Masks(m, &masks);
  EXPECT_EQ(masks[1], Rollback::InputGate::ALL);
  EXPECT_TRUE(masks[0].main_stick);
  const Tile& tile = TileFor(s->progress.picked);
  m.Write32(TASK + 0x228, tile.page);
  Hover(m, s->progress.picked);
  bool pressed = false;
  for (int i = 0; i < 3; ++i)
  {
    Boundary(m);
    masks = {};
    Masks(m, &masks);
    pressed = pressed || masks[0].press == PAD_BUTTON_A;
  }
  EXPECT_TRUE(pressed);
  // The game takes another stage in that frame: the flow's stands.
  m.Write32(TASK + 0x224, 2);
  m.Write32(TASK + 0x258, s->progress.picked == Battlefield ? 0x02 : 0x01);
  Boundary(m);
  EXPECT_EQ(StageForStageId(static_cast<int>(m.Read32(TASK + 0x258))), s->progress.picked);

  // The fight counts a game: the next stage select's coin is another.
  Scene(m, "scMelee");
  Boundary(m);
  EXPECT_EQ(ReadState(m)->set.game, 2);
  Scene(m, "scVsResult");
  Boundary(m);
  EXPECT_EQ(ReadState(m)->set.game, 2);
  EXPECT_EQ(ReadState(m)->score[0] + ReadState(m)->score[1], 0);
  Scene(m, "scSelStage");
  m.Write32(TASK + 0x224, 0);
  m.Write32(TASK + 0x250, 0);
  Boundary(m);
  s = ReadState(m);
  EXPECT_EQ(s->seed, RS::CasualSeed(ROOM, 1, 2));
  EXPECT_EQ(s->progress.prefs[0], -1);
  EXPECT_EQ(s->progress.prefs[1], -1);
  EXPECT_EQ(s->progress.step, 0);
}

TEST_F(OrcaBrawlCasual, NoFinalDestinationForAnIceClimbersAndNothingAlone)
{
  FakeMemory m = Game("scSelStage");
  m.Write8(MELEE + 0x98, 0x10);  // port 1 plays Ice Climbers
  Boundary(m);
  const auto s = ReadState(m);
  ASSERT_TRUE(s);
  EXPECT_FALSE(RS::Allowed(CasualPlanOf(*s), s->progress) & (1 << FinalDestination));
  Hover(m, FinalDestination);
  Press(m, 1, BrawlStages::GF_X);
  EXPECT_EQ(ReadState(m)->progress.prefs[1], -1);
  // FD's tile is grey.
  m.Write32(TASK + 0x228, 0);
  Boundary(m);
  EXPECT_EQ(IconFrame(m, TileFor(FinalDestination).slot), 0u);
  EXPECT_NE(IconFrame(m, TileFor(Battlefield).slot), 0u);
  // The opponent gone: the stage select is the player's own again, its cursor anyone's.
  EXPECT_EQ(m.Read32(TASK + 0x278), 1u);
  Frame(m, false);
  EXPECT_FALSE(ReadState(m));
  EXPECT_EQ(m.Read32(TASK + 0x278), 0xF0u);
  Rollback::InputGate::Masks masks{};
  Masks(m, &masks);
  for (const auto& mask : masks)
    EXPECT_TRUE(mask.Empty());
}
