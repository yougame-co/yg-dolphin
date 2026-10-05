// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// No CPUs online, and only the seats in use (Core/Orca/UX/OnlineSeats.h; ORCA.md "No CPUs
// online"): which selects are online, what the record and the panels keep there, which panels are
// hidden, and where A is kept off the player-type buttons, over a fake of the game's memory.

#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "Core/Orca/UX/MatchBlock.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/OnlineRules.h"
#include "Core/Orca/UX/OnlineSeats.h"
#include "Core/Orca/UX/Queue.h"
#include "InputCommon/GCPadStatus.h"

using namespace Orca::UX;
using namespace Orca::UX::OnlineSeats;
namespace MB = Orca::UX::MatchBlock;
using Rules::Header;
using Rules::Mode;
using Rules::Ruleset;

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
  void Put32(u32 a, u32 v)
  {
    for (u32 i = 0; i < 4; ++i)
      bytes[a + i] = static_cast<u8>(v >> (24 - 8 * i));
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

// Where the fake game keeps things (the shapes are the game's, the addresses made up).
constexpr u32 MANAGER = 0x80900000;
constexpr u32 SCENE = 0x80910000;
constexpr u32 SCENE_NAME = 0x80920000;
constexpr u32 SEQUENCE = 0x80921000;
constexpr u32 SEQUENCE_NAME = 0x80922000;
constexpr u32 GLOBAL = 0x90181300;
constexpr u32 RECORD = 0x90180B40;  // gmSelCharData, where both games keep it
constexpr u32 TASK = 0x80930000;
constexpr u32 AREA = 0x80940000;    // + port x 0x1000
constexpr u32 HAND = 0x80950000;    // + port x 0x100
constexpr u32 OBJECTS = 0x80960000;  // + port x 0x1000 + slot x 0x40: the MuObjects
constexpr u32 MODELS = 0x80980000;   // + port x 0x4000 + slot x 0x100: their ScnMdls
constexpr u32 VTABLE = 0x80470E40;

constexpr u32 FLAGS_GAME = 0xA0000000;
constexpr u32 FLAGS_HIDDEN = 0xA0000060;

u32 ModelOf(int port, u32 slot)
{
  return MODELS + static_cast<u32>(port) * 0x4000 + slot * 0x100;
}
u32 FlagsOf(const FakeMemory& m, int port, u32 slot)
{
  return m.Read32(ModelOf(port, slot) + 0xCC);
}
u8 RecordState(const FakeMemory& m, int port)
{
  return m.Read8(RECORD + 0xB8 + static_cast<u32>(port) * 0x5C + 1);
}
void SetRecordState(FakeMemory& m, int port, u8 character, u8 state)
{
  const u32 p = RECORD + 0xB8 + static_cast<u32>(port) * 0x5C;
  m.bytes[p] = character;
  m.bytes[p + 1] = state;
}
u32 Kind(const FakeMemory& m, int port)
{
  return m.Read32(AREA + static_cast<u32>(port) * 0x1000 + 0x1B4);
}
void SetKind(FakeMemory& m, int port, u32 kind)
{
  m.Put32(AREA + static_cast<u32>(port) * 0x1000 + 0x1B4, kind);
}
void SetScene(FakeMemory& m, std::string_view scene)
{
  m.Fill(SCENE_NAME, 0x20);
  m.Text(SCENE_NAME, scene);
}
void SetPick(FakeMemory& m, u32 pick)
{
  m.Put32(SEQUENCE + 0x18, pick);
}
void SetHand(FakeMemory& m, int port, u32 target, float y, u32 button = 0)
{
  const u32 hand = HAND + static_cast<u32>(port) * 0x100;
  m.Put32(hand + 0x80, target);
  m.Put32(hand + 0x90, std::bit_cast<u32>(0.0f));
  m.Put32(hand + 0x94, std::bit_cast<u32>(y));
  m.Put32(hand + 0xAC, button);
}

// The Versus character select as both games build it: four player areas, each with its hand and
// 40 model slots (slot 36 empty, as the game leaves it), each MuObject's scene model with the
// game's flags; gmSelCharData with nobody in it; the match block holding the game's dead code;
// sqVsMelee running with the menu's pick `pick` (the heap's fill: Group > Brawl).
FakeMemory Game(std::string_view scene = "scSelctCharacter", u32 pick = 0xCCCCCCCC)
{
  FakeMemory m;
  m.Put32(0x805A0060, MANAGER);
  m.Fill(MANAGER, 0x300);
  m.Put32(MANAGER + 0x4, SCENE);
  m.Put32(MANAGER + 0x10, SEQUENCE);
  m.Fill(SCENE, 0x410);
  m.Put32(SCENE, SCENE_NAME);
  SetScene(m, scene);
  m.Fill(SEQUENCE, 0x20, 0xCC);
  m.Put32(SEQUENCE, SEQUENCE_NAME);
  m.Text(SEQUENCE_NAME, "sqVsMelee");
  SetPick(m, pick);
  m.Put32(0x805A00E0, GLOBAL);
  m.Fill(GLOBAL, 0x40);
  m.Put32(GLOBAL + 0x10, RECORD);
  m.Fill(RECORD, 0xB8 + 8 * 0x5C);
  for (int port = 0; port < 8; ++port)
    SetRecordState(m, port, 0x3E, 3);
  m.Put32(SCENE + 0x400, TASK);
  m.Fill(TASK, 0x60);
  for (u32 port = 0; port < 4; ++port)
  {
    const u32 area = AREA + port * 0x1000;
    m.Put32(TASK + 0x44 + 4 * port, area);
    m.Fill(area, 0x200);
    m.Put32(area + 0x1A8, HAND + port * 0x100);
    m.Fill(HAND + port * 0x100, 0xE0);
    SetHand(m, static_cast<int>(port), Rules::CSS_HAND_GRID, 5.0f);
    for (u32 slot = 0; slot < 40; ++slot)
    {
      if (slot == 36)
        continue;
      const u32 object = OBJECTS + port * 0x1000 + slot * 0x40;
      const u32 model = ModelOf(static_cast<int>(port), slot);
      m.Put32(area + 0xB0 + 4 * slot, object);
      m.Fill(object, 0x40);
      m.Put32(object + 0xC, model);
      m.Fill(model, 0x100);
      m.Put32(model, VTABLE);
      m.Put32(model + 0xCC, FLAGS_GAME);
    }
  }
  for (u32 i = 0; i < MB::FULL_SIZE; i += 4)
    m.Put32(MB::BASE + i, 0x3A200000 + i);  // the game's dead code
  m.writes = 0;
  return m;
}

// The same under a header (the queue's own character select with `flags` FLAG_SOLO | FLAG_QUEUE2,
// a queue room's with FLAG_QUEUE2).
FakeMemory QueueSelect(Mode mode, u8 flags, std::string_view scene = "scSelctCharacter")
{
  FakeMemory m = Game(scene, mode == Mode::Ranked ? PICK_RANKED : PICK_CASUAL);
  Rules::WriteHeader(m, mode, Ruleset::PPlus, 0, 0, flags);
  m.writes = 0;
  return m;
}
constexpr u8 SOLO = MB::FLAG_SOLO | MB::FLAG_QUEUE2;
constexpr u8 ROOM = MB::FLAG_QUEUE2;

u8 HiddenAfter(FakeMemory& m, u8 plugged)
{
  u8 hidden = 0;
  ApplySeats(m, plugged, &hidden);
  return hidden;
}
}  // namespace

TEST(OrcaOnlineSeats, WhatIsOnline)
{
  const Header none{};
  Header solo{true, Mode::Ranked, Ruleset::Brawl, SOLO};
  Header room{true, Mode::Casual, Ruleset::PPlus, ROOM};
  Header cleared{true, Mode::None, Ruleset::Brawl};
  // A header that locks: the queue's own select and a queue room, whatever the pick and ports.
  EXPECT_TRUE(Online(solo, 0, 1));
  EXPECT_TRUE(Online(room, 0, 2));
  // With Friends (24-27), Casual (30) and Ranked (31), before any header.
  for (u32 pick : {24u, 25u, 26u, 27u, 30u, 31u})
    EXPECT_TRUE(Online(none, pick, 1)) << pick;
  // Group > Brawl (the heap's fill, or 0 once the back out used it), the Spectator code, anything
  // else: local, alone.
  for (u32 pick : {0u, 0xCCCCCCCCu, 1u, 23u, 28u, 29u, 32u})
  {
    EXPECT_FALSE(Online(none, pick, 1)) << pick;
    EXPECT_FALSE(Online(cleared, pick, 1)) << pick;
  }
  // A friend plugged in, wherever the host was.
  EXPECT_TRUE(Online(none, 0, 2));
  EXPECT_TRUE(Online(none, 0xCCCCCCCC, 4));
  EXPECT_FALSE(Online(none, 0, 0));
}

TEST(OrcaOnlineSeats, TheRecordKeepsNoCpuAndNoHumanWithoutAPort)
{
  constexpr u8 HUMAN = STATE_HUMAN, CPU = STATE_CPU, NOBODY = STATE_NOBODY;
  EXPECT_EQ(RecordStateFor(CPU, true), NOBODY);
  EXPECT_EQ(RecordStateFor(CPU, false), NOBODY);
  EXPECT_EQ(RecordStateFor(HUMAN, false), NOBODY);
  EXPECT_EQ(RecordStateFor(HUMAN, true), HUMAN);
  EXPECT_EQ(RecordStateFor(NOBODY, false), NOBODY);
  EXPECT_EQ(RecordStateFor(NOBODY, true), NOBODY);
  // Anything else this doesn't know stays.
  EXPECT_EQ(RecordStateFor(2, false), 2);
  EXPECT_EQ(PanelKindFor(AREA_KIND_CPU), AREA_KIND_NONE);
  EXPECT_EQ(PanelKindFor(AREA_KIND_HUMAN), AREA_KIND_HUMAN);
  EXPECT_EQ(PanelKindFor(AREA_KIND_NONE), AREA_KIND_NONE);
}

TEST(OrcaOnlineSeats, ACpuFromLocalVersusNeverReachesRankedsCharacterSelect)
{
  // Project+ after a local game that reached the stage select: port 1 a human (Ike), port 2 a
  // second local player's human, port 3 a CPU (Random), port 4 nobody; then PLAY ONLINE > Ranked.
  // Between the menu and the character select (scMemoryChange) the pick is in sqVsMelee.
  FakeMemory m = Game("scMemoryChange", PICK_RANKED);
  SetRecordState(m, 0, 0x1E, STATE_HUMAN);
  SetRecordState(m, 1, 0x06, STATE_HUMAN);
  SetRecordState(m, 2, 0x29, STATE_CPU);
  // Entries 5-7 (the record has more than four) are never touched.
  SetRecordState(m, 4, 0x29, STATE_CPU);
  SetRecordState(m, 6, 0x10, STATE_HUMAN);
  EXPECT_EQ(ClearRecord(m, 0x1), 2);
  EXPECT_EQ(RecordState(m, 0), STATE_HUMAN);   // this player, plugged in
  EXPECT_EQ(RecordState(m, 1), STATE_NOBODY);  // nobody's port
  EXPECT_EQ(RecordState(m, 2), STATE_NOBODY);  // the CPU
  EXPECT_EQ(RecordState(m, 3), STATE_NOBODY);
  EXPECT_EQ(RecordState(m, 4), STATE_CPU);
  EXPECT_EQ(RecordState(m, 6), STATE_HUMAN);
  // The characters stay (the panel builds from the state alone).
  EXPECT_EQ(m.Read8(RECORD + 0xB8 + 2 * 0x5C), 0x29);
  // Idempotent: run again at the same boundary (a re-run), nothing.
  m.writes = 0;
  EXPECT_EQ(ClearRecord(m, 0x1), 0);
  EXPECT_EQ(m.writes, 0);
}

TEST(OrcaOnlineSeats, TheRecordIsClearedOnlyBetweenScenes)
{
  // The stage select's end copies the record into the fight's players: never there, never in a
  // fight or on the results screen, nor on the character select (which saves its own panels).
  for (std::string_view scene : {"scSelctCharacter", "scSelStage", "scMelee", "scVsResult",
                                 "muMenuMain"})
  {
    FakeMemory m = Game(scene, PICK_RANKED);
    SetRecordState(m, 2, 0x29, STATE_CPU);
    EXPECT_EQ(ClearRecord(m, 0x1), 0) << scene;
    EXPECT_EQ(RecordState(m, 2), STATE_CPU) << scene;
  }
}

TEST(OrcaOnlineSeats, EveryOnlineModeClearsTheRecordAndLocalVersusKeepsItsCpus)
{
  struct Case
  {
    const char* what;
    u32 pick;
    u8 plugged;
    std::optional<std::pair<Mode, u8>> header;
    bool cleared;
  };
  const Case cases[] = {
      {"Group > Brawl", 0xCCCCCCCC, 0x1, std::nullopt, false},
      {"Group > Brawl, the back out's 0", 0, 0x1, std::nullopt, false},
      {"With Friends", 25, 0x1, std::nullopt, true},
      {"With Friends with a friend", 25, 0x3, std::nullopt, true},
      {"Casual before its header", PICK_CASUAL, 0x1, std::nullopt, true},
      {"Ranked before its header", PICK_RANKED, 0x1, std::nullopt, true},
      {"a friend dropped into Group > Brawl", 0xCCCCCCCC, 0x3, std::nullopt, true},
      {"the queue's own select", PICK_RANKED, 0x1, std::pair{Mode::Ranked, SOLO}, true},
      {"a casual queue room", 0, 0x3, std::pair{Mode::Casual, ROOM}, true},
      {"a ranked queue room", 0, 0x3, std::pair{Mode::Ranked, ROOM}, true},
  };
  for (const Case& c : cases)
  {
    FakeMemory m = Game("scMemoryChange", c.pick);
    if (c.header)
      Rules::WriteHeader(m, c.header->first, Ruleset::Brawl, 0, 0, c.header->second);
    SetRecordState(m, 0, 0x1E, STATE_HUMAN);
    SetRecordState(m, 2, 0x06, STATE_CPU);
    SetRecordState(m, 3, 0x29, STATE_CPU);
    ClearRecord(m, c.plugged);
    EXPECT_EQ(RecordState(m, 2), c.cleared ? STATE_NOBODY : STATE_CPU) << c.what;
    EXPECT_EQ(RecordState(m, 3), c.cleared ? STATE_NOBODY : STATE_CPU) << c.what;
    EXPECT_EQ(RecordState(m, 0), STATE_HUMAN) << c.what;
  }
}

TEST(OrcaOnlineSeats, AFriendsHumanStaysWhileTheyArePluggedIn)
{
  FakeMemory m = Game("scMemoryChange", 25);
  SetRecordState(m, 0, 0x1E, STATE_HUMAN);
  SetRecordState(m, 1, 0x06, STATE_HUMAN);
  SetRecordState(m, 2, 0x10, STATE_HUMAN);
  // Ports 1 and 2 plugged in; port 3's friend left.
  EXPECT_EQ(ClearRecord(m, 0x3), 1);
  EXPECT_EQ(RecordState(m, 0), STATE_HUMAN);
  EXPECT_EQ(RecordState(m, 1), STATE_HUMAN);
  EXPECT_EQ(RecordState(m, 2), STATE_NOBODY);
}

TEST(OrcaOnlineSeats, ACpuPanelOnAnOnlineSelectBecomesEmpty)
{
  // With Friends' select with a CPU on panel 3 (a friend's drop-in onto a select that had one).
  FakeMemory m = Game("scSelctCharacter", 25);
  SetKind(m, 0, AREA_KIND_HUMAN);
  SetKind(m, 1, AREA_KIND_HUMAN);
  SetKind(m, 2, AREA_KIND_CPU);
  EXPECT_EQ(ClearPanels(m, 0x3), 4);
  EXPECT_EQ(Kind(m, 0), AREA_KIND_HUMAN);
  EXPECT_EQ(Kind(m, 1), AREA_KIND_HUMAN);
  EXPECT_EQ(Kind(m, 2), AREA_KIND_NONE);
  m.writes = 0;
  EXPECT_EQ(ClearPanels(m, 0x3), 0);
  EXPECT_EQ(m.writes, 0);
  // Local Versus keeps its CPU; and nothing off the character select.
  FakeMemory local = Game("scSelctCharacter");
  SetKind(local, 2, AREA_KIND_CPU);
  EXPECT_EQ(ClearPanels(local, 0x1), 0);
  EXPECT_EQ(Kind(local, 2), AREA_KIND_CPU);
  FakeMemory between = Game("scMemoryChange", PICK_RANKED);
  SetKind(between, 2, AREA_KIND_CPU);
  EXPECT_EQ(ClearPanels(between, 0x1), 0);
  // A queue room (any pick: the header locks).
  FakeMemory room = QueueSelect(Mode::Ranked, ROOM);
  SetKind(room, 1, AREA_KIND_CPU);
  EXPECT_EQ(ClearPanels(room, 0x1), 4);
  EXPECT_EQ(Kind(room, 1), AREA_KIND_NONE);
}

TEST(OrcaOnlineSeats, WhichSeatsAreHidden)
{
  const Header none{};
  const Header solo{true, Mode::Ranked, Ruleset::Brawl, SOLO};
  const Header room{true, Mode::Casual, Ruleset::Brawl, ROOM};
  // Offline: never.
  for (int port = 0; port < 4; ++port)
    EXPECT_FALSE(SeatHidden(false, none, port, AREA_KIND_NONE, false, false));
  // The queue's own select, picking: only the player's own panel.
  EXPECT_FALSE(SeatHidden(true, solo, 0, AREA_KIND_HUMAN, true, false));
  for (int port = 1; port < 4; ++port)
    EXPECT_TRUE(SeatHidden(true, solo, port, AREA_KIND_NONE, false, false)) << port;
  // Searching: the opponent's seat shows (SEARCHING and the ring), 3 and 4 don't.
  EXPECT_FALSE(SeatHidden(true, solo, 1, AREA_KIND_NONE, false, true));
  EXPECT_TRUE(SeatHidden(true, solo, 2, AREA_KIND_NONE, false, true));
  EXPECT_TRUE(SeatHidden(true, solo, 3, AREA_KIND_NONE, false, true));
  // A queue room: both players' seats, from before the opponent plugs in.
  EXPECT_FALSE(SeatHidden(true, room, 1, AREA_KIND_NONE, false, false));
  EXPECT_FALSE(SeatHidden(true, room, 1, AREA_KIND_HUMAN, true, false));
  EXPECT_TRUE(SeatHidden(true, room, 2, AREA_KIND_NONE, false, false));
  EXPECT_TRUE(SeatHidden(true, room, 3, AREA_KIND_NONE, false, false));
  // With Friends: whoever is plugged in; a friend's empty panel shows the moment they plug in.
  EXPECT_TRUE(SeatHidden(true, none, 1, AREA_KIND_NONE, false, false));
  EXPECT_FALSE(SeatHidden(true, none, 1, AREA_KIND_NONE, true, false));
  EXPECT_FALSE(SeatHidden(true, none, 3, AREA_KIND_NONE, true, false));
  // Never a panel someone plays: a human who left keeps theirs, and a CPU's shows until it is
  // cleared (ClearPanels runs first at the same boundary).
  EXPECT_FALSE(SeatHidden(true, none, 1, AREA_KIND_HUMAN, false, false));
  EXPECT_FALSE(SeatHidden(true, room, 2, AREA_KIND_CPU, false, false));
}

TEST(OrcaOnlineSeats, TheQueuesOwnSelectShowsThePlayerAloneThenTheSearchingSeat)
{
  FakeMemory m = QueueSelect(Mode::Ranked, SOLO);
  SetKind(m, 0, AREA_KIND_HUMAN);
  EXPECT_EQ(HiddenAfter(m, 0x1), 0b1110);
  // 39 models a panel, each one word.
  for (u32 slot = 0; slot < 40; ++slot)
  {
    if (slot == 36)
      continue;
    EXPECT_EQ(FlagsOf(m, 0, slot), FLAGS_GAME) << slot;
    for (int port = 1; port < 4; ++port)
      EXPECT_EQ(FlagsOf(m, port, slot), FLAGS_HIDDEN) << port << " " << slot;
  }
  // Idempotent.
  m.writes = 0;
  EXPECT_EQ(ApplySeats(m, 0x1), 0);
  EXPECT_EQ(m.writes, 0);
  // Ready (searching): the opponent's seat comes back, whole.
  m.Write8(Orca::UX::Queue::READY, 1);
  m.writes = 0;
  EXPECT_EQ(ApplySeats(m, 0x1), 39);
  for (u32 slot = 0; slot < 40; ++slot)
  {
    if (slot != 36)
      EXPECT_EQ(FlagsOf(m, 1, slot), FLAGS_GAME) << slot;
  }
  u8 hidden = 0;
  ApplySeats(m, 0x1, &hidden);
  EXPECT_EQ(hidden, 0b1100);
}

TEST(OrcaOnlineSeats, AQueueRoomShowsBothPlayers)
{
  FakeMemory m = QueueSelect(Mode::Casual, ROOM);
  SetKind(m, 0, AREA_KIND_HUMAN);
  EXPECT_EQ(HiddenAfter(m, 0x1), 0b1100);
  SetKind(m, 1, AREA_KIND_HUMAN);
  EXPECT_EQ(HiddenAfter(m, 0x3), 0b1100);
}

TEST(OrcaOnlineSeats, WithFriendsShowsAFriendsPanelAsTheyPlugIn)
{
  FakeMemory m = Game("scSelctCharacter", 25);
  SetKind(m, 0, AREA_KIND_HUMAN);
  EXPECT_EQ(HiddenAfter(m, 0x1), 0b1110);
  // Port 3's friend plugs in: their panel shows, still empty until their hand reaches the grid.
  EXPECT_EQ(HiddenAfter(m, 0x5), 0b1010);
  EXPECT_EQ(FlagsOf(m, 2, 0), FLAGS_GAME);
  EXPECT_EQ(FlagsOf(m, 1, 0), FLAGS_HIDDEN);
}

TEST(OrcaOnlineSeats, ASelectThatStopsBeingOnlineShowsEveryPanelAgain)
{
  // A friend dropped into Group > Brawl (two ports: online), then left (one: local again).
  FakeMemory m = Game("scSelctCharacter");
  SetKind(m, 0, AREA_KIND_HUMAN);
  EXPECT_EQ(HiddenAfter(m, 0x3), 0b1100);
  EXPECT_EQ(HiddenAfter(m, 0x1), 0);
  for (int port = 0; port < 4; ++port)
    EXPECT_EQ(FlagsOf(m, port, 5), FLAGS_GAME);
  // Local Versus from the start writes nothing at all.
  FakeMemory local = Game("scSelctCharacter");
  EXPECT_EQ(ApplySeats(local, 0x1), 0);
  EXPECT_EQ(local.writes, 0);
}

TEST(OrcaOnlineSeats, OnlyModelsThisKnowsAreTouched)
{
  FakeMemory m = Game("scSelctCharacter", 25);
  // Flags that aren't the game's, a scene model without a vtable in the executable, a pointer that
  // goes nowhere: left alone.
  m.Put32(ModelOf(2, 3) + 0xCC, 0xA0000001);
  m.Put32(ModelOf(2, 4), 0x00000000);
  m.Put32(OBJECTS + 2 * 0x1000 + 5 * 0x40 + 0xC, 0x81234560);
  m.Put32(AREA + 2 * 0x1000 + 0xB0 + 4 * 6, 0x81234560);
  ApplySeats(m, 0x1);
  EXPECT_EQ(FlagsOf(m, 2, 3), 0xA0000001u);
  EXPECT_EQ(FlagsOf(m, 2, 4), FLAGS_GAME);
  EXPECT_EQ(FlagsOf(m, 2, 5), FLAGS_GAME);
  EXPECT_EQ(FlagsOf(m, 2, 6), FLAGS_GAME);
  EXPECT_EQ(FlagsOf(m, 2, 7), FLAGS_HIDDEN);
  // Off the character select, nothing (the models are the select's, built fresh each time).
  FakeMemory sss = Game("scSelStage", 25);
  EXPECT_EQ(ApplySeats(sss, 0x1), 0);
  EXPECT_EQ(sss.writes, 0);
}

TEST(OrcaOnlineSeats, APlayerTypeButtonNeverTakesAOnAnOnlineSelectWithoutAHeader)
{
  using Rules::CssHand;
  // In the grid, on a token, on the panel's picture (a costume), above the buttons: A goes.
  EXPECT_TRUE(HandMayPressA(CssHand{true, Rules::CSS_HAND_GRID, 0, 0, 5.0f}));
  EXPECT_TRUE(HandMayPressA(CssHand{true, Rules::CSS_HAND_BUTTON, 0x1B, 0, -10.0f}));
  EXPECT_TRUE(HandMayPressA(CssHand{true, Rules::CSS_HAND_EXIT, 0x02, -28, 19.0f}));
  EXPECT_TRUE(HandMayPressA(CssHand{true, Rules::CSS_HAND_NOTHING, 0, 0, -16.4f}));
  // On a player-type or name button, or within a frame's travel of them: no A.
  EXPECT_FALSE(HandMayPressA(CssHand{true, Rules::CSS_HAND_BUTTON, 0x1D, 0, -19.8f}));
  EXPECT_FALSE(HandMayPressA(CssHand{true, Rules::CSS_HAND_BUTTON, 0x1C, 0, -19.8f}));
  EXPECT_FALSE(HandMayPressA(CssHand{true, Rules::CSS_HAND_NOTHING, 0, 0, -17.0f}));
  EXPECT_FALSE(HandMayPressA(CssHand{true, Rules::CSS_HAND_BUTTON, 0x1B, 0, -16.5f}));
  EXPECT_FALSE(
      HandMayPressA(CssHand{true, Rules::CSS_HAND_NOTHING, 0, 0,
                            std::numeric_limits<float>::quiet_NaN()}));
  // A hand that can't be read: the game's own A, as before.
  EXPECT_TRUE(HandMayPressA(CssHand{}));
}

TEST(OrcaOnlineSeats, TheGateKeepsAOffTheButtonsOnlyWhereItShould)
{
  const auto a_masked = [](const FakeMemory& m, int port) {
    return (GateMasks(m)[port].buttons & PAD_BUTTON_A) != 0;
  };
  // With Friends: port 1 on panel 3's player-type button, port 2 in the grid.
  FakeMemory m = Game("scSelctCharacter", 25);
  SetHand(m, 0, Rules::CSS_HAND_BUTTON, -19.8f, Rules::CSS_BUTTON_PLAYER_TYPE);
  EXPECT_TRUE(a_masked(m, 0));
  EXPECT_FALSE(a_masked(m, 1));
  // Nothing else is masked by this rule.
  EXPECT_EQ(GateMasks(m)[0].buttons, PAD_BUTTON_A);
  EXPECT_TRUE(GateMasks(m)[1].Empty());
  // Group > Brawl: the game's own A, CPUs and all.
  FakeMemory local = Game("scSelctCharacter");
  SetHand(local, 0, Rules::CSS_HAND_BUTTON, -19.8f, Rules::CSS_BUTTON_PLAYER_TYPE);
  EXPECT_FALSE(a_masked(local, 0));
  // ...until a friend has plugged in (the friends bytes the frame hook keeps, FriendsMove).
  local.bytes[MB::FRIENDS_TAG] = MB::FRIENDS_TAG_VALUE;
  local.bytes[MB::FRIENDS_SEEN] = 0x3;
  EXPECT_TRUE(a_masked(local, 0));
  local.bytes[MB::FRIENDS_SEEN] = 0x1;
  EXPECT_FALSE(a_masked(local, 0));
  // Under a header the online rules decide (A only in the grid): nothing from here.
  FakeMemory solo = QueueSelect(Mode::Ranked, SOLO);
  SetHand(solo, 0, Rules::CSS_HAND_BUTTON, -19.8f, Rules::CSS_BUTTON_PLAYER_TYPE);
  EXPECT_TRUE(GateMasks(solo)[0].Empty());
  // Off the character select: nothing.
  FakeMemory sss = Game("scSelStage", 25);
  EXPECT_TRUE(GateMasks(sss)[0].Empty());
}

TEST(OrcaOnlineSeats, PluggedInMemoryReadsTheFriendsBytes)
{
  FakeMemory m = Game();
  EXPECT_EQ(PluggedInMemory(m), 0x1);
  m.bytes[MB::FRIENDS_SEEN] = 0x7;
  EXPECT_EQ(PluggedInMemory(m), 0x1);  // not Orca's yet (no tag)
  m.bytes[MB::FRIENDS_TAG] = MB::FRIENDS_TAG_VALUE;
  EXPECT_EQ(PluggedInMemory(m), 0x7);
}
