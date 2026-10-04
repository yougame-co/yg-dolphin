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
#include "Core/Orca/UX/SetBlock.h"
#include "InputCommon/GCPadStatus.h"

using namespace Orca::UX;
using namespace Orca::UX::Rules;
namespace MB = Orca::UX::MatchBlock;

namespace
{
int NextFrame()
{
  static int s_frame = 0;
  return ++s_frame;
}

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
constexpr u32 PPLUS_RSS = 0x8042C4E8;
constexpr u32 PPLUS_STRIKES = 0x8042C822;

void SetScene(FakeMemory& m, std::string_view scene)
{
  m.Fill(SCENE_NAME, 0x20);
  m.Text(SCENE_NAME, scene);
}

// A port's pick in the match's init data, as the character select writes it: type 0 a human's
// pick, 1 a CPU's, 3 nobody's; the character (gmCharacterKind; 0x3E none).
void SetPick(FakeMemory& m, int port, u8 type, u8 character)
{
  const u32 p = MELEE + 0x98 + static_cast<u32>(port) * 0x5C;
  m.Write8(p + 0, character);
  m.Write8(p + 1, type);
}
constexpr u8 HUMAN = 0, CPU = 1, NOBODY = 3, NONE = 0x3E;

// Brawl's memory as the probes found it: the scene manager and its scene, g_GameGlobal with the
// rules and the match's init data, the record's menu data with the fresh save's defaults, and the
// match block holding the game's dead code.
FakeMemory Game(std::string_view scene)
{
  FakeMemory m;
  m.Fill(0x805A0060, 4);
  m.Fill(0x80900000, 0x10);
  m.Fill(SCENE, 0x410);
  m.Write32(0x805A0060, 0x80900000);
  m.Write32(0x80900004, SCENE);
  m.Write32(SCENE, SCENE_NAME);
  SetScene(m, scene);
  m.Fill(0x805A00E0, 4);
  m.Fill(0x90181300, 0x40);
  m.Write32(0x805A00E0, 0x90181300);
  m.Write32(0x90181308, MELEE);
  m.Write32(0x9018131C, SET_RULE);
  m.Fill(MELEE, 0x98 + 4 * 0x5C);
  for (int port = 0; port < 4; ++port)
    SetPick(m, port, NOBODY, NONE);
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
  for (u32 i = 0; i < MB::FULL_SIZE; i += 4)
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
  // The dead code is gone from the rest of the block: zeros but the saved rules and a fresh set
  // (the stage flow's "none" ports).
  EXPECT_EQ(m.Read32(MB::SET + 0x40), 0u);
  EXPECT_EQ(m.Read8(MB::SET + 0x03), MB::NONE);
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
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(SET_RULE + 0x0A), 0);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x08), 0u);
  EXPECT_GT(WriteHeader(m, Mode::None, Ruleset::Brawl), 0);
  EXPECT_FALSE(ReadHeader(m).present);
  EXPECT_EQ(m.Read8(SET_RULE + 0x02), 0x00);
  EXPECT_EQ(m.Read8(SET_RULE + 0x0A), 1);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x00), 0x02000000u);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x08), 0xFFFFFFFFu);
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), 0x7FFFFFFFu);
  // The block is the dead code's first word, then zeros: nothing locks, clearing again writes
  // nothing, and a new header may go there.
  EXPECT_EQ(m.Read32(MB::BASE), 0x3A200000u);
  for (u32 i = 4; i < MB::FULL_SIZE; ++i)
    ASSERT_EQ(m.Read8(MB::BASE + i), 0) << i;
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
  m.writes = 0;
  EXPECT_EQ(WriteHeader(m, Mode::None, Ruleset::Brawl), 0);
  EXPECT_EQ(m.writes, 0);
  EXPECT_GT(WriteHeader(m, Mode::Casual, Ruleset::Brawl), 0);
  EXPECT_TRUE(ReadHeader(m).Locked());
}

TEST(OrcaOnlineRules, NoHeaderBeforeTheGameLoadsTheModule)
{
  // Before sora_scene is loaded the block's memory is something else's (zeros at boot).
  FakeMemory m = Game("scSelctCharacter");
  for (u32 i = 0; i < MB::FULL_SIZE; ++i)
    m.Write8(MB::BASE + i, 0);
  m.writes = 0;
  EXPECT_EQ(WriteHeader(m, Mode::Ranked, Ruleset::Brawl, 1, 7), 0);
  EXPECT_EQ(m.writes, 0);
}

TEST(OrcaOnlineRules, AnotherQueueRoomStartsANewSetAndKeepsTheSavedRules)
{
  FakeMemory m = Game("scSelctCharacter");
  WriteHeader(m, Mode::Casual, Ruleset::Brawl);
  ApplyLocks(m, NextFrame());
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
  EXPECT_GT(ApplyLocks(m, NextFrame()), 0);
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
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
  EXPECT_EQ(m.writes, 0);
  // A player's change on the rules screen goes back at the next boundary.
  m.Write8(SET_RULE + 0x04, 5);
  m.Write8(MENU_DATA, 3);
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 2);
  EXPECT_EQ(m.Read8(SET_RULE + 0x04), 3);
  EXPECT_EQ(m.Read8(MENU_DATA), 0);
}

TEST(OrcaOnlineRules, RankedChoosesTheStageAndProjectPlusPlaysFourStocks)
{
  FakeMemory ranked = Locked(Mode::Ranked, Ruleset::Brawl);
  ApplyLocks(ranked, NextFrame());
  EXPECT_EQ(ranked.Read8(SET_RULE + 0x07), 0);  // Choose
  EXPECT_EQ(ranked.Read8(SET_RULE + 0x04), 3);
  FakeMemory pplus = Locked(Mode::Casual, Ruleset::PPlus);
  ApplyLocks(pplus, NextFrame());
  EXPECT_EQ(pplus.Read8(SET_RULE + 0x04), 4);
  EXPECT_EQ(pplus.Read8(SET_RULE + 0x07), 1);
  // Project+'s stage list is its own: Brawl's Random Stage Switch is left alone.
  EXPECT_EQ(pplus.Read32(MENU_DATA + 0x24), 0x7FFFFFFFu);
}

TEST(OrcaOnlineRules, IceClimbersTakeFinalDestinationOutOfTheRandomStages)
{
  FakeMemory m = Locked(Mode::Casual, Ruleset::Brawl);
  SetPick(m, 0, HUMAN, 0x00);  // Mario
  SetPick(m, 1, HUMAN, 0x02);  // Link
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24) & BRAWL_FINAL_DESTINATION_BIT, BRAWL_FINAL_DESTINATION_BIT);
  SetPick(m, 1, HUMAN, CHARACTER_ICE_CLIMBERS);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE & ~BRAWL_FINAL_DESTINATION_BIT);
  // A CPU's Ice Climbers don't count (and a queue match has none).
  SetPick(m, 1, CPU, CHARACTER_ICE_CLIMBERS);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE);
  // On the way to the fight (a Random character resolved there).
  SetScene(m, "scSelStage");
  SetPick(m, 1, HUMAN, CHARACTER_ICE_CLIMBERS);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE & ~BRAWL_FINAL_DESTINATION_BIT);
  // In the fight the list stays as it was.
  SetScene(m, "scMelee");
  SetPick(m, 1, HUMAN, 0x02);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(MENU_DATA + 0x24), BRAWL_LEGAL_BRAWL_PAGE & ~BRAWL_FINAL_DESTINATION_BIT);
}

TEST(OrcaOnlineRules, ProjectPlusTakesThe2024ProposedListOnTheCharacterSelect)
{
  FakeMemory m = Locked(Mode::Casual, Ruleset::PPlus);
  // The player's own preset was saved with the header.
  EXPECT_EQ(m.Read8(MB::SAVED_RSS), 0x11);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read32(PPLUS_RSS), 0x00000003u);  // page 0's random mask: the 9 stages
  EXPECT_EQ(m.Read8(PPLUS_RSS + 0x3C), 9);      // page 0 holds 9 stages
  EXPECT_EQ(m.Read8(PPLUS_RSS + 0xA8), 0x11);   // past the list: untouched
  EXPECT_EQ(m.Read8(PPLUS_STRIKES), 0);         // casual: no strikes left over
  // Ranked keeps the strike table to its stage select.
  FakeMemory ranked = Locked(Mode::Ranked, Ruleset::PPlus);
  ApplyLocks(ranked, NextFrame());
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
  SetPick(m, 0, HUMAN, CHARACTER_ICE_CLIMBERS);
  m.writes = 0;
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
  EXPECT_EQ(m.writes, 0);
  // A header with mode none (never written so, but a reader must not lock on it).
  m.Write32(MB::MAGIC, MB::MAGIC_VALUE);
  m.Write8(MB::VERSION, MB::VERSION_VALUE);
  m.Write8(MB::MODE, 0);
  m.Write8(MB::RULESET, 1);
  m.writes = 0;
  EXPECT_EQ(ApplyLocks(m, NextFrame()), 0);
  EXPECT_EQ(m.writes, 0);
  // Another version's block is no block.
  m.Write8(MB::MODE, 1);
  m.Write8(MB::VERSION, 9);
  EXPECT_FALSE(ReadHeader(m).present);
}

TEST(OrcaOnlineRules, RankedsReadyTimerFindsTheNoShow)
{
  FakeMemory m = Locked(Mode::Ranked, Ruleset::Brawl);
  m.Write8(MB::PLUGGED, 3);
  SetPick(m, 0, HUMAN, 0x00);  // ready: Mario
  for (int f = 0; f < CSS_READY_FRAMES; ++f)
    ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), CSS_READY_FRAMES - 1);
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 0);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 2);  // port 2
  // Picking now changes nothing: the no-show stands while the character select is up.
  SetPick(m, 1, HUMAN, 0x02);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 2);
  // Leaving the character select clears it.
  SetScene(m, "scMemoryChange");
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 0);
}

TEST(OrcaOnlineRules, TheReadyTimerWaitsForBothPlayersAndStopsWhenBothPick)
{
  FakeMemory m = Locked(Mode::Ranked, Ruleset::Brawl);
  SetPick(m, 0, HUMAN, 0x00);
  // The opponent isn't plugged in yet: no timer.
  m.Write8(MB::PLUGGED, 1);
  for (int f = 0; f < CSS_READY_FRAMES + 5; ++f)
    ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 0);
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 0);
  m.Write8(MB::PLUGGED, 3);
  for (int f = 0; f < 101; ++f)
    ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 100);
  SetPick(m, 1, HUMAN, 0x02);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 0);
  // Nobody picked yet: no timer either.
  SetPick(m, 0, NOBODY, NONE);
  SetPick(m, 1, NOBODY, NONE);
  ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 0);
  // After a fight the set's later games keep their characters: no no-show.
  SetScene(m, "scMelee");
  ApplyLocks(m, NextFrame());
  SetScene(m, "scSelctCharacter");
  SetPick(m, 0, HUMAN, 0x00);
  for (int f = 0; f < CSS_READY_FRAMES + 5; ++f)
    ApplyLocks(m, NextFrame());
  EXPECT_EQ(m.Read8(MB::CSS_NO_SHOW), 0);
  // Casual has no timer.
  FakeMemory c = Locked(Mode::Casual, Ruleset::Brawl);
  c.Write8(MB::PLUGGED, 3);
  SetPick(c, 0, HUMAN, 0x00);
  for (int f = 0; f < CSS_READY_FRAMES + 5; ++f)
    ApplyLocks(c, NextFrame());
  EXPECT_EQ(c.Read16(MB::CSS_TIMER), 0);
  EXPECT_EQ(c.Read8(MB::CSS_NO_SHOW), 0);
}

TEST(OrcaOnlineRules, TheReadyTimerIsTheSameWhenABoundaryRunsAgain)
{
  // A boundary run again after a load (the hook at the same frame over memory it already wrote)
  // leaves the timer where the first run did.
  FakeMemory m = Locked(Mode::Ranked, Ruleset::Brawl);
  m.Write8(MB::PLUGGED, 3);
  SetPick(m, 0, HUMAN, 0x00);
  for (int f = 1000; f < 1100; ++f)
    ApplyLocks(m, f);
  const auto once = m.bytes;
  EXPECT_EQ(ApplyLocks(m, 1099), 0);
  EXPECT_EQ(m.bytes, once);
  EXPECT_EQ(m.Read16(MB::CSS_TIMER), 99);
}

TEST(OrcaOnlineRules, TheHeaderCarriesTheCoinAndANewRoomStartsANewSet)
{
  FakeMemory m = Game("scSelctCharacter");
  EXPECT_GT(WriteHeader(m, Mode::Ranked, Ruleset::Brawl, 1, 0x1234), 0);
  EXPECT_EQ(m.Read8(MB::COIN), 1);
  EXPECT_EQ(m.Read32(MB::ROOM), 0x1234u);
  // Every track's set starts fresh: the stage flow's and the ranked set's "none" ports.
  const auto state = Orca::UX::MatchBlock::Read(m);
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->coin, 1);
  EXPECT_EQ(state->last_winner, Orca::UX::MatchBlock::NONE);
  EXPECT_EQ(Orca::UX::SetBlock::ReadSet(m).last_winner, Orca::UX::SetBlock::NO_PORT);
  // The same room again: nothing to write.
  EXPECT_EQ(WriteHeader(m, Mode::Ranked, Ruleset::Brawl, 1, 0x1234), 0);
  // A set in progress, then another room with the same mode: a new set.
  Orca::UX::SetBlock::SetState set;
  set.games = 1;
  set.wins = {1, 0};
  set.last_winner = 0;
  Orca::UX::SetBlock::WriteSet(m, set);
  EXPECT_GT(WriteHeader(m, Mode::Ranked, Ruleset::Brawl, 0, 0x5678), 0);
  EXPECT_EQ(Orca::UX::SetBlock::ReadSet(m), Orca::UX::SetBlock::SetState{});
  EXPECT_EQ(m.Read8(MB::COIN), 0);
}

TEST(OrcaOnlineRules, TheTestQueueKnobTakesACoin)
{
  using Orca::UX::Rules::ParseTestQueue;
  using Orca::UX::Rules::TestQueue;
  EXPECT_EQ(ParseTestQueue("ranked"), (TestQueue{Mode::Ranked, 0}));
  EXPECT_EQ(ParseTestQueue("ranked:1"), (TestQueue{Mode::Ranked, 1}));
  EXPECT_EQ(ParseTestQueue("casual:0"), (TestQueue{Mode::Casual, 0}));
  EXPECT_FALSE(ParseTestQueue("ranked:2").has_value());
  EXPECT_FALSE(ParseTestQueue("brawl").has_value());
  EXPECT_FALSE(ParseTestQueue("").has_value());
}

TEST(OrcaOnlineRules, LocksArePureFunctionsOfMemory)
{
  // The same memory twice (a first run and its re-run from the snapshot) writes the same bytes.
  FakeMemory a = Locked(Mode::Casual, Ruleset::Brawl);
  FakeMemory b = Locked(Mode::Casual, Ruleset::Brawl);
  SetPick(a, 0, HUMAN, CHARACTER_ICE_CLIMBERS);
  SetPick(b, 0, HUMAN, CHARACTER_ICE_CLIMBERS);
  EXPECT_EQ(ApplyLocks(a, 500), ApplyLocks(b, 500));
  EXPECT_EQ(a.bytes, b.bytes);
}
