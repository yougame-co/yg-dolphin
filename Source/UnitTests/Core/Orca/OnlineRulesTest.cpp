// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The online rules for queue matches, Phase 1 (Core/Orca/UX/OnlineRules.h): the match block's
// header and what it locks, over a fake of the game's memory.

#include <map>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "Core/Orca/UX/MatchBlock.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/OnlineRules.h"
#include "InputCommon/GCPadStatus.h"

using namespace Orca::UX;
using namespace Orca::UX::Rules;
namespace MB = Orca::UX::MatchBlock;

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

constexpr u32 SET_RULE = 0x9017F360;
constexpr u32 MELEE = 0x90180F20;
constexpr u32 MENU_DATA = 0x9017BE50;
constexpr u32 SCENE = 0x80910000;
constexpr u32 SCENE_NAME = 0x80920000;
constexpr u32 CSS_TASK = 0x80930000;
constexpr u32 PANELS = 0x80940000;
constexpr u32 PPLUS_RSS = 0x8042C4E8;
constexpr u32 PPLUS_STRIKES = 0x8042C822;

void SetScene(FakeMemory& m, std::string_view scene)
{
  m.Fill(SCENE_NAME, 0x20);
  m.Text(SCENE_NAME, scene);
}

// A panel on the character select: kind 0 human, 1 CPU, 2 closed; the character's id or none.
void SetPanel(FakeMemory& m, int port, u32 kind, u32 character)
{
  const u32 panel = PANELS + static_cast<u32>(port) * 0x200;
  m.Write32(panel + 0x1B4, kind);
  m.Write32(panel + 0x1B8, character);
}

// Brawl's memory as the probes found it: the scene manager and its scene (the character select's
// task and panels), g_GameGlobal with the rules and the match's init data, the record's menu data
// with the fresh save's defaults, and the match block holding the game's dead code.
FakeMemory Game(std::string_view scene)
{
  FakeMemory m;
  m.Fill(0x805A0060, 4);
  m.Fill(0x80900000, 0x10);
  m.Fill(SCENE, 0x410);
  m.Write32(0x805A0060, 0x80900000);
  m.Write32(0x80900004, SCENE);
  m.Write32(SCENE, SCENE_NAME);
  m.Write32(SCENE + 0x400, CSS_TASK);
  SetScene(m, scene);
  m.Fill(CSS_TASK, 0x60);
  m.Fill(PANELS, 4 * 0x200);
  for (int port = 0; port < 4; ++port)
  {
    m.Write32(CSS_TASK + 0x44 + 4 * static_cast<u32>(port),
              PANELS + static_cast<u32>(port) * 0x200);
    SetPanel(m, port, 2, 0xFFFFFFFF);
  }
  m.Fill(0x805A00E0, 4);
  m.Fill(0x90181300, 0x40);
  m.Write32(0x805A00E0, 0x90181300);
  m.Write32(0x90181308, MELEE);
  m.Write32(0x9018131C, SET_RULE);
  m.Fill(MELEE, 0x98 + 4 * 0x5C);
  for (u32 port = 0; port < 4; ++port)
    m.Write8(MELEE + 0x98 + port * 0x5C + 1, 3);  // nobody
  m.Fill(SET_RULE, 0x88);
  // The fresh save's rules: a 2-minute timed match, 3 stocks, ratio 1.0, Choose, pause on.
  m.Write8(SET_RULE + 0x02, 0x00);
  m.Write8(SET_RULE + 0x03, 2);
  m.Write8(SET_RULE + 0x04, 3);
  m.Write8(SET_RULE + 0x06, 10);
  m.Write8(SET_RULE + 0x0A, 1);
  m.Write8(SET_RULE + 0x0C, 1);
  m.Fill(MENU_DATA, 0x30);
  m.Write32(MENU_DATA + 0x00, 0x02000000);  // Medium
  m.Write32(MENU_DATA + 0x04, 0x4F524341);  // 'ORCA'
  m.Write32(MENU_DATA + 0x08, 0xFFFFFFFF);
  m.Write32(MENU_DATA + 0x0C, 0xFFFFFFFF);
  m.Write32(MENU_DATA + 0x20, 0x0003FF00);  // every Melee stage
  m.Write32(MENU_DATA + 0x24, 0x7FFFFFFF);  // every Brawl stage
  for (u32 i = 0; i < MB::SIZE; i += 4)
    m.Write32(MB::BASE + i, 0x3A200000 + i);  // the game's dead code
  m.Fill(PPLUS_RSS, 0x320, 0x11);
  m.Fill(PPLUS_STRIKES, 0x1E, 0x01);
  m.writes = 0;
  return m;
}

FakeMemory Locked(Mode mode, Ruleset ruleset, std::string_view scene = "scSelctCharacter")
{
  FakeMemory m = Game(scene);
  WriteHeader(m, mode, ruleset);
  m.writes = 0;
  return m;
}
}  // namespace

TEST(OrcaOnlineRules, NoHeaderInTheGamesDeadCode)
{
  FakeMemory m = Game("scSelctCharacter");
  EXPECT_FALSE(ReadHeader(m).present);
  EXPECT_FALSE(ReadHeader(m).Locked());
  // Clearing what isn't there touches nothing.
  EXPECT_EQ(WriteHeader(m, Mode::None, Ruleset::Brawl), 0);
  EXPECT_EQ(m.writes, 0);
  EXPECT_EQ(m.Read32(MB::BASE), 0x3A200000u);
}

TEST(OrcaOnlineRules, TheHeaderIsWrittenOnceAndSavesThePlayersRules)
{
  FakeMemory m = Game("scSelctCharacter");
  EXPECT_GT(WriteHeader(m, Mode::Casual, Ruleset::Brawl), 0);
  const Header h = ReadHeader(m);
  EXPECT_TRUE(h.Locked());
  EXPECT_EQ(h.mode, Mode::Casual);
  EXPECT_EQ(h.ruleset, Ruleset::Brawl);
  EXPECT_EQ(m.Read32(MB::MAGIC), MB::MAGIC_VALUE);
  // The dead code is gone from the rest of the block: zeros but the saved rules.
  EXPECT_EQ(m.Read32(MB::SET), 0u);
  EXPECT_EQ(m.Read8(MB::SAVED_RULES + 1), 2);  // gmSetRule +0x03
  EXPECT_EQ(m.Read32(MB::SAVED_ITEMS), 0x02000000u);
  EXPECT_EQ(m.Read32(MB::SAVED_STAGES + 4), 0x7FFFFFFFu);
  // Twice is once.
  m.writes = 0;
  EXPECT_EQ(WriteHeader(m, Mode::Casual, Ruleset::Brawl), 0);
  EXPECT_EQ(m.writes, 0);
}

TEST(OrcaOnlineRules, ClearingGivesBackThePlayersRules)
{
  FakeMemory m = Game("scSelctCharacter");
  WriteHeader(m, Mode::Ranked, Ruleset::Brawl);
  ApplyLocks(m);
  EXPECT_EQ(m.Read8(SET_RULE + 0x0A), 0);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x08), 0u);
  EXPECT_GT(WriteHeader(m, Mode::None, Ruleset::Brawl), 0);
  EXPECT_FALSE(ReadHeader(m).present);
  EXPECT_EQ(m.Read8(SET_RULE + 0x02), 0x00);
  EXPECT_EQ(m.Read8(SET_RULE + 0x0A), 1);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x00), 0x02000000u);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x08), 0xFFFFFFFFu);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), 0x7FFFFFFFu);
  // The block is all zeros: nothing locks, and clearing again writes nothing.
  for (u32 i = 0; i < MB::SIZE; ++i)
    ASSERT_EQ(m.Read8(MB::BASE + i), 0) << i;
  EXPECT_EQ(ApplyLocks(m), 0);
  m.writes = 0;
  EXPECT_EQ(WriteHeader(m, Mode::None, Ruleset::Brawl), 0);
  EXPECT_EQ(m.writes, 0);
}

TEST(OrcaOnlineRules, AnotherQueueRoomStartsANewSetAndKeepsTheSavedRules)
{
  FakeMemory m = Game("scSelctCharacter");
  WriteHeader(m, Mode::Casual, Ruleset::Brawl);
  ApplyLocks(m);
  m.Write8(MB::SET, 0x55);
  m.Write16(MB::CSS_TIMER, 77);
  EXPECT_GT(WriteHeader(m, Mode::Ranked, Ruleset::Brawl), 0);
  EXPECT_EQ(ReadHeader(m).mode, Mode::Ranked);
  EXPECT_EQ(m.Read8(MB::SET), 0);
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 0);
  // The player's own, not the casual lock's.
  EXPECT_EQ(m.Read32(MB::SAVED_ITEMS), 0x02000000u);
  EXPECT_EQ(m.Read8(MB::SAVED_RULES + 8), 1);  // gmSetRule +0x0A, pause on
}

TEST(OrcaOnlineRules, BrawlsCasualRules)
{
  FakeMemory m = Locked(Mode::Casual, Ruleset::Brawl);
  EXPECT_GT(ApplyLocks(m), 0);
  EXPECT_EQ(m.Read8(SET_RULE + 0x02) & 7, 1);  // stock
  EXPECT_EQ(m.Read8(SET_RULE + 0x04), 3);
  EXPECT_EQ(m.Read8(SET_RULE + 0x05), 0);
  EXPECT_EQ(m.Read8(SET_RULE + 0x06), 10);
  EXPECT_EQ(m.Read8(SET_RULE + 0x07), 1);  // Random
  EXPECT_EQ(m.Read8(SET_RULE + 0x08), 8);
  EXPECT_EQ(m.Read8(SET_RULE + 0x09), 1);
  EXPECT_EQ(m.Read8(SET_RULE + 0x0A), 0);
  EXPECT_EQ(m.Read8(SET_RULE + 0x03), 2);  // a timed match's minutes: not the lock's
  EXPECT_EQ(m.Read8(SET_RULE + 0x0C), 1);  // the damage gauge: not the lock's
  EXPECT_EQ(m.Read8(MENU_DATA), 0);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x04), 0x4F524341u);  // the patches' marker stays
  EXPECT_EQ(m.Read32(MENU_DATA + 0x08), 0u);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x0C), 0u);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x20), BRAWL_LEGAL_MELEE_PAGE);
  // Idempotent: a re-run at the same boundary writes nothing.
  m.writes = 0;
  EXPECT_EQ(ApplyLocks(m), 0);
  EXPECT_EQ(m.writes, 0);
  // A player's change on the rules screen goes back at the next boundary.
  m.Write8(SET_RULE + 0x04, 5);
  m.Write8(MENU_DATA, 3);
  EXPECT_EQ(ApplyLocks(m), 2);
  EXPECT_EQ(m.Read8(SET_RULE + 0x04), 3);
  EXPECT_EQ(m.Read8(MENU_DATA), 0);
}

TEST(OrcaOnlineRules, RankedChoosesTheStageAndProjectPlusPlaysFourStocks)
{
  FakeMemory ranked = Locked(Mode::Ranked, Ruleset::Brawl);
  ApplyLocks(ranked);
  EXPECT_EQ(ranked.Read8(SET_RULE + 0x07), 0);  // Choose
  EXPECT_EQ(ranked.Read8(SET_RULE + 0x04), 3);
  FakeMemory pplus = Locked(Mode::Casual, Ruleset::PPlus);
  ApplyLocks(pplus);
  EXPECT_EQ(pplus.Read8(SET_RULE + 0x04), 4);
  EXPECT_EQ(pplus.Read8(SET_RULE + 0x07), 1);
  // Project+'s stage list is its own: Brawl's Random Stage Switch is left alone.
  EXPECT_EQ(pplus.Read32(MENU_DATA + 0x24), 0x7FFFFFFFu);
}

TEST(OrcaOnlineRules, IceClimbersTakeFinalDestinationOutOfTheRandomStages)
{
  FakeMemory m = Locked(Mode::Casual, Ruleset::Brawl);
  SetPanel(m, 0, 0, 0x00);  // Mario
  SetPanel(m, 1, 0, 0x02);  // Link
  ApplyLocks(m);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24) & BRAWL_FINAL_DESTINATION_BIT, BRAWL_FINAL_DESTINATION_BIT);
  SetPanel(m, 1, 0, CHARACTER_ICE_CLIMBERS);
  ApplyLocks(m);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE & ~BRAWL_FINAL_DESTINATION_BIT);
  // A CPU's Ice Climbers don't count (and a queue match has none).
  SetPanel(m, 1, 1, CHARACTER_ICE_CLIMBERS);
  ApplyLocks(m);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE);
  // After the character select: the match's init data, where a Random character is resolved.
  SetScene(m, "scSelStage");
  m.Write8(MELEE + 0x98 + 0 * 0x5C + 0, 0x00);
  m.Write8(MELEE + 0x98 + 0 * 0x5C + 1, 0);
  m.Write8(MELEE + 0x98 + 1 * 0x5C + 0, CHARACTER_ICE_CLIMBERS);
  m.Write8(MELEE + 0x98 + 1 * 0x5C + 1, 0);
  ApplyLocks(m);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE & ~BRAWL_FINAL_DESTINATION_BIT);
  // In the fight the list stays as it was.
  SetScene(m, "scMelee");
  m.Write8(MELEE + 0x98 + 1 * 0x5C + 0, 0x02);
  ApplyLocks(m);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE & ~BRAWL_FINAL_DESTINATION_BIT);
}

TEST(OrcaOnlineRules, ProjectPlusTakesThe2024ProposedListOnTheCharacterSelect)
{
  FakeMemory m = Locked(Mode::Casual, Ruleset::PPlus);
  // The player's own preset was saved with the header.
  EXPECT_EQ(m.Read8(MB::SAVED_RSS), 0x11);
  ApplyLocks(m);
  EXPECT_EQ(m.Read32(PPLUS_RSS), 0x00000003u);  // page 0's random mask: the 9 stages
  EXPECT_EQ(m.Read8(PPLUS_RSS + 0x3C), 9);      // page 0 holds 9 stages
  EXPECT_EQ(m.Read8(PPLUS_RSS + 0xA8), 0x11);   // past the list: untouched
  EXPECT_EQ(m.Read8(PPLUS_STRIKES), 0);         // casual: no strikes left over
  // Ranked keeps the strike table to its stage select.
  FakeMemory ranked = Locked(Mode::Ranked, Ruleset::PPlus);
  ApplyLocks(ranked);
  EXPECT_EQ(ranked.Read8(PPLUS_STRIKES), 0x01);
  // Cleared: the player's preset comes back.
  WriteHeader(m, Mode::None, Ruleset::PPlus);
  EXPECT_EQ(m.Read8(PPLUS_RSS), 0x11);
  EXPECT_EQ(m.Read8(PPLUS_RSS + 0x3C), 0x11);
}

TEST(OrcaOnlineRules, TheGateLocksTheMenusOnlyUnderAHeader)
{
  FakeMemory m = Game("scSelctCharacter");
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.Empty());
  WriteHeader(m, Mode::Casual, Ruleset::Brawl);
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons, PAD_BUTTON_B);
  SetScene(m, "scSelStage");
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons, PAD_BUTTON_B);
  SetScene(m, "scMelee");
  for (const auto& mask : GateMasks(m))
    EXPECT_EQ(mask.buttons, PAD_BUTTON_START);
  SetScene(m, "scVsResult");
  for (const auto& mask : GateMasks(m))
    EXPECT_TRUE(mask.Empty());
  FakeMemory p = Locked(Mode::Ranked, Ruleset::PPlus);
  for (const auto& mask : GateMasks(p))
  {
    EXPECT_EQ(mask.buttons, PAD_BUTTON_B | PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT |
                                PAD_BUTTON_RIGHT);
    EXPECT_FALSE(mask.main_stick);
  }
  SetScene(p, "scSelStage");
  for (const auto& mask : GateMasks(p))
    EXPECT_EQ(mask.buttons, PAD_BUTTON_B | PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_TRIGGER_Z);
}

TEST(OrcaOnlineRules, FriendsGamesAreUntouched)
{
  FakeMemory m = Game("scSelctCharacter");
  SetPanel(m, 0, 0, CHARACTER_ICE_CLIMBERS);
  EXPECT_EQ(ApplyLocks(m), 0);
  EXPECT_EQ(m.writes, 0);
  // A header with mode none (never written so, but a reader must not lock on it).
  m.Write32(MB::MAGIC, MB::MAGIC_VALUE);
  m.Write8(MB::VERSION, MB::VERSION_VALUE);
  m.Write8(MB::MODE, 0);
  m.Write8(MB::RULESET, 1);
  m.writes = 0;
  EXPECT_EQ(ApplyLocks(m), 0);
  EXPECT_EQ(m.writes, 0);
  // Another version's block is no block.
  m.Write8(MB::MODE, 1);
  m.Write8(MB::VERSION, 9);
  EXPECT_FALSE(ReadHeader(m).present);
}

TEST(OrcaOnlineRules, RankedsReadyTimerFindsTheNoShow)
{
  FakeMemory m = Locked(Mode::Ranked, Ruleset::Brawl);
  SetPanel(m, 0, 0, 0x00);         // ready: Mario
  SetPanel(m, 1, 0, 0xFFFFFFFF);   // a human with no character yet
  for (int f = 0; f < CSS_READY_FRAMES - 1; ++f)
    ApplyLocks(m);
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), CSS_READY_FRAMES - 1);
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 0);
  ApplyLocks(m);
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 2);  // port 2
  // Picking now changes nothing: the no-show stands while the character select is up.
  SetPanel(m, 1, 0, 0x02);
  ApplyLocks(m);
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 2);
  // Leaving the character select clears it.
  SetScene(m, "scMemoryChange");
  ApplyLocks(m);
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 0);
}

TEST(OrcaOnlineRules, TheReadyTimerStopsWhenBothPickAndAfterTheFirstFight)
{
  FakeMemory m = Locked(Mode::Ranked, Ruleset::Brawl);
  SetPanel(m, 0, 0, 0x00);
  SetPanel(m, 1, 0, 0xFFFFFFFF);
  for (int f = 0; f < 100; ++f)
    ApplyLocks(m);
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 100);
  SetPanel(m, 1, 0, 0x02);
  ApplyLocks(m);
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 0);
  // Nobody picked yet, or only one human: no timer either.
  SetPanel(m, 0, 0, 0xFFFFFFFF);
  SetPanel(m, 1, 0, 0xFFFFFFFF);
  ApplyLocks(m);
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 0);
  // After a fight the set's later games keep their characters: no no-show.
  SetScene(m, "scMelee");
  ApplyLocks(m);
  SetScene(m, "scSelctCharacter");
  SetPanel(m, 0, 0, 0x00);
  for (int f = 0; f < CSS_READY_FRAMES + 5; ++f)
    ApplyLocks(m);
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 0);
  // Casual has no timer.
  FakeMemory c = Locked(Mode::Casual, Ruleset::Brawl);
  SetPanel(c, 0, 0, 0x00);
  SetPanel(c, 1, 0, 0xFFFFFFFF);
  for (int f = 0; f < CSS_READY_FRAMES + 5; ++f)
    ApplyLocks(c);
  EXPECT_EQ(c.Read16(MB::CSS_TIMER), 0);
  EXPECT_EQ(c.Read8(MB::CSS_NO_SHOW), 0);
}

TEST(OrcaOnlineRules, LocksArePureFunctionsOfMemory)
{
  // The same memory twice (a first run and its re-run from the snapshot) writes the same bytes.
  FakeMemory a = Locked(Mode::Casual, Ruleset::Brawl);
  FakeMemory b = Locked(Mode::Casual, Ruleset::Brawl);
  SetPanel(a, 0, 0, CHARACTER_ICE_CLIMBERS);
  SetPanel(b, 0, 0, CHARACTER_ICE_CLIMBERS);
  EXPECT_EQ(ApplyLocks(a), ApplyLocks(b));
  EXPECT_EQ(a.bytes, b.bytes);
}
