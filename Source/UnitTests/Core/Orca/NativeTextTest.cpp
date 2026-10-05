// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/UX/CharOrder.h"
#include "Core/Orca/UX/MatchBlock.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/NativeText.h"
#include "Core/Orca/UX/Queue.h"
#include "Core/Orca/UX/SetBlock.h"

using namespace Orca::UX;
namespace NT = Orca::UX::NativeText;
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
  void Put32(u32 a, u32 v)
  {
    for (int i = 0; i < 4; ++i)
      bytes[a + i] = static_cast<u8>(v >> (24 - 8 * i));
  }
  void PutBytes(u32 a, const std::vector<u8>& v)
  {
    for (size_t i = 0; i < v.size(); ++i)
      bytes[a + static_cast<u32>(i)] = v[i];
  }
  void PutString(u32 a, std::string_view s)
  {
    for (size_t i = 0; i < s.size(); ++i)
      bytes[a + static_cast<u32>(i)] = static_cast<u8>(s[i]);
    bytes[a + static_cast<u32>(s.size())] = 0;
  }
  std::map<u32, u8> bytes;
  int writes = 0;
};

// The set-ups as the game formats them (harness RAM dumps, 2026-10-04): a name plate's and the
// rules bar's, each 43 bytes.
const std::vector<u8> kPlateSetup{0x17, 0xff, 0xbf, 0x00, 0x04, 0x00, 0x3e, 0xff, 0xf0, 0x18, 0x01,
                                  0x03, 0x18, 0x10, 0x30, 0x1d, 0x20, 0x1c, 0x00, 0xcc, 0x0d, 0x40,
                                  0x40, 0x0f, 0xff, 0x00, 0x05, 0x01, 0x02, 0x02, 0x0c, 0xed, 0xce,
                                  0xd0, 0xff, 0x04, 0xed, 0xce, 0xd0, 0xff, 0x03, 0x00, 0x00};
const std::vector<u8> kRulesSetup{0x17, 0xff, 0x48, 0x00, 0x12, 0x00, 0xb8, 0xff, 0xee, 0x18, 0x00,
                                  0x03, 0x18, 0x10, 0x30, 0x1d, 0x20, 0x1c, 0x00, 0xcc, 0x0d, 0x40,
                                  0x40, 0x0f, 0xff, 0x00, 0x05, 0x01, 0x02, 0x01, 0x0c, 0xff, 0xff,
                                  0xff, 0xff, 0x04, 0xff, 0xff, 0xff, 0xff, 0x03, 0x00, 0x00};
// The rules bar's message for a stock battle: a width, a colour, the words, the colour's end and a
// width again.
const std::vector<u8> kRulesBefore{0x11, 0x29, 0xff, 0xff, 0xff, 0x12, 0x02, 0x0c, 0x56, 0x54, 0x4f, 0xff};
const std::vector<u8> kRulesAfter{0x13, 0x11, 0x00, 0x00, 0x00, 0x00};

constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 MANAGER = 0x805B8BA0;
constexpr u32 SCENE = 0x90FF6340;
constexpr u32 NAMES = 0x80701000;
constexpr u32 TASK = 0x815E75E0;
constexpr u32 AREAS = 0x81400000;  // area p at AREAS + p * 0x500
constexpr u32 BOXES = 0x81500000;  // box k at BOXES + k * 0x200 (its buffer at +0x80)
constexpr u32 RULES_BOX = 4;

u32 BoxAt(u32 k)
{
  return BOXES + k * 0x200;
}

// A message buffer object `k` holding `bytes` (the set-up and the message), with its end byte.
void PutBox(FakeMemory& m, u32 k, const std::vector<u8>& bytes, u32 capacity = 256)
{
  const u32 object = BoxAt(k);
  const u32 buffer = object + 0x80;
  m.Fill(object, 0x80);
  m.Fill(buffer, capacity, 0xCC);
  m.Put32(object, NT::MESSAGE_BUFFER_VTABLE);
  m.Put32(object + NT::BUFFER_CAPACITY, capacity);
  m.Put32(object + NT::BUFFER_LENGTH, static_cast<u32>(bytes.size()));
  m.Put32(object + NT::BUFFER_DATA, buffer);
  m.PutBytes(buffer, bytes);
  m.bytes[buffer + static_cast<u32>(bytes.size())] = NT::END;
}

std::vector<u8> Concat(std::initializer_list<std::vector<u8>> parts)
{
  std::vector<u8> out;
  for (const auto& p : parts)
    out.insert(out.end(), p.begin(), p.end());
  return out;
}

std::vector<u8> Bytes(std::string_view s)
{
  return {s.begin(), s.end()};
}

std::vector<u8> RulesMessage(std::string_view words)
{
  return Concat({kRulesSetup, kRulesBefore, Bytes(words), kRulesAfter});
}

// The Versus character select: four player areas (`joined` bit p: a human in area p), each with
// its plate ("PLAYER n" or "  NONE"), and the rules bar.
FakeMemory Css(u8 joined)
{
  FakeMemory m;
  m.Put32(SCENE_MANAGER, MANAGER);
  m.Fill(MANAGER, 0x40);
  m.Put32(MANAGER + 4, SCENE);
  m.Fill(SCENE, 0x420);
  m.Put32(SCENE, NAMES);
  m.PutString(NAMES, "scSelctCharacter");
  m.Put32(SCENE + 0x400, TASK);
  m.Fill(TASK, 0x660);
  for (u32 p = 0; p < 4; ++p)
  {
    const u32 area = AREAS + p * 0x500;
    m.Fill(area, 0x448);
    m.Put32(TASK + 0x44 + 4 * p, area);
    m.Put32(area + 0x1B0, p);
    const bool human = (joined >> p) & 1;
    m.Put32(area + 0x1B4, human ? 1 : 0);
    m.Put32(area + 0x1B8, 0x28);  // no character
    m.Put32(area + 0x174, BoxAt(p));
    PutBox(m, p,
           Concat({kPlateSetup, Bytes(human ? "PLAYER " + std::to_string(p + 1) : "  NONE")}));
  }
  m.Put32(TASK + 0x538, BoxAt(RULES_BOX));
  PutBox(m, RULES_BOX, RulesMessage("-man survival test!"));
  return m;
}

// The match block with a queue header (`flags`: MB::FLAG_QUEUE2, FLAG_SOLO), casual or ranked.
void PutHeader(FakeMemory& m, u8 flags, bool ranked)
{
  for (u32 i = 0; i < MB::FULL_SIZE; ++i)
    m.bytes[MB::BASE + i] = 0;
  m.Write32(MB::MAGIC, MB::MAGIC_VALUE);
  m.Write8(MB::VERSION, MB::VERSION_VALUE);
  m.Write8(MB::MODE, ranked ? MB::MODE_RANKED : MB::MODE_CASUAL);
  m.Write8(MB::RULESET, 1);
  m.Write8(MB::FLAGS, flags);
}

Orca::Events::PortInfo Port(int port, std::string name, int rating = -1, bool remote = false)
{
  Orca::Events::PortInfo p;
  p.port = port;
  p.name = std::move(name);
  p.remote = remote;
  if (rating >= 0)
  {
    Queue::Identity id;
    id.rating = rating;
    p.queue = Queue::EncodeIdentity(id);
  }
  return p;
}

std::string WordsOf(const FakeMemory& m, u32 k)
{
  const auto box = NT::ReadBox(m, BoxAt(k));
  return box ? NT::Words(m, *box) : "<no box>";
}
}  // namespace

TEST(OrcaNativeText, ReadsThePlatesAndTheRulesBarAsTheGameFormatsThem)
{
  const FakeMemory m = Css(0b0011);
  EXPECT_EQ(WordsOf(m, 0), "PLAYER 1");
  EXPECT_EQ(WordsOf(m, 1), "PLAYER 2");
  EXPECT_EQ(WordsOf(m, 2), "  NONE");
  // The rules bar's codes around its words are not words.
  const auto rules = NT::ReadBox(m, BoxAt(RULES_BOX));
  ASSERT_TRUE(rules);
  EXPECT_EQ(rules->words, kRulesSetup.size() + kRulesBefore.size());
  EXPECT_EQ(NT::Words(m, *rules), "-man survival test!");
}

TEST(OrcaNativeText, RejectsAnythingElse)
{
  FakeMemory m = Css(1);
  EXPECT_FALSE(NT::ReadBox(m, 0));
  EXPECT_FALSE(NT::ReadBox(m, BoxAt(0) + 2));  // unaligned
  FakeMemory wrong_vtable = m;
  wrong_vtable.Put32(BoxAt(0), 0x80454000);
  EXPECT_FALSE(NT::ReadBox(wrong_vtable, BoxAt(0)));
  FakeMemory no_end = m;
  no_end.bytes[BoxAt(0) + 0x80 + 51] = 0xCC;
  EXPECT_FALSE(NT::ReadBox(no_end, BoxAt(0)));
  FakeMemory not_a_setup = m;
  not_a_setup.bytes[BoxAt(0) + 0x80] = 0x18;
  EXPECT_FALSE(NT::ReadBox(not_a_setup, BoxAt(0)));
  FakeMemory too_long = m;
  too_long.Put32(BoxAt(0) + NT::BUFFER_LENGTH, 256);
  EXPECT_FALSE(NT::ReadBox(too_long, BoxAt(0)));
}

TEST(OrcaNativeText, SetWordsKeepsTheCodesAndTheLength)
{
  FakeMemory m = Css(1);
  const auto box = NT::ReadBox(m, BoxAt(RULES_BOX));
  ASSERT_TRUE(box);
  EXPECT_EQ(NT::SetWords(m, *box, "Press Start to lock in  0:27"), 1);
  const u32 buffer = BoxAt(RULES_BOX) + 0x80;
  const std::vector<u8> want = RulesMessage("Press Start to lock in  0:27");
  for (size_t i = 0; i < want.size(); ++i)
    EXPECT_EQ(m.Read8(buffer + static_cast<u32>(i)), want[i]) << i;
  EXPECT_EQ(m.Read8(buffer + static_cast<u32>(want.size())), NT::END);
  EXPECT_EQ(m.Read32(BoxAt(RULES_BOX) + NT::BUFFER_LENGTH), want.size());
  // Read again, it is the same box with the new words; the same words again write nothing.
  const auto again = NT::ReadBox(m, BoxAt(RULES_BOX));
  ASSERT_TRUE(again);
  EXPECT_EQ(NT::Words(m, *again), "Press Start to lock in  0:27");
  const int writes = m.writes;
  EXPECT_EQ(NT::SetWords(m, *again, "Press Start to lock in  0:27"), 0);
  EXPECT_EQ(m.writes, writes);
  // Shorter: the length follows, the codes move up.
  EXPECT_EQ(NT::SetWords(m, *again, "Both locked in"), 1);
  EXPECT_EQ(WordsOf(m, RULES_BOX), "Both locked in");
  EXPECT_EQ(m.Read32(BoxAt(RULES_BOX) + NT::BUFFER_LENGTH), RulesMessage("Both locked in").size());
}

TEST(OrcaNativeText, SetWordsNeverPassesTheCapacity)
{
  FakeMemory m;
  PutBox(m, 0, Concat({kPlateSetup, Bytes("PLAYER 1")}), 56);
  const auto box = NT::ReadBox(m, BoxAt(0));
  ASSERT_TRUE(box);
  EXPECT_EQ(NT::SetWords(m, *box, "ADA 1532"), 1);  // 43 + 8 + the end: 52
  const auto now = NT::ReadBox(m, BoxAt(0));
  ASSERT_TRUE(now);
  const int writes = m.writes;
  EXPECT_EQ(NT::SetWords(m, *now, "SANDBOX-ADA 1532"), 0);  // 43 + 16 + 1 > 56
  EXPECT_EQ(m.writes, writes);
  EXPECT_EQ(WordsOf(m, 0), "ADA 1532");
}

TEST(OrcaNativeText, TextTheFontCanDraw)
{
  EXPECT_EQ(NT::Ascii("ada · 1532"), "ada - 1532");
  EXPECT_EQ(NT::Ascii("1–0"), "1-0");
  EXPECT_EQ(NT::Ascii("Searching…"), "Searching...");
  EXPECT_EQ(NT::Ascii("café ✓!"), "caf !");
  EXPECT_EQ(NT::Ascii("abcdef", 4), "abcd");
  EXPECT_EQ(NT::PlateName("sandbox_ada"), "SANDBOX-AD");
  EXPECT_EQ(NT::PlateName("bo"), "BO");
  EXPECT_EQ(NT::PlateName("éé"), "");
  EXPECT_EQ(NT::Clock(30 * 60), "0:30");
  EXPECT_EQ(NT::Clock(30 * 60 - 1), "0:30");
  EXPECT_EQ(NT::Clock(29 * 60), "0:29");
  EXPECT_EQ(NT::Clock(0), "0:00");
  EXPECT_EQ(NT::Clock(-5), "0:00");
  EXPECT_EQ(NT::Clock(45 * 60), "0:45");
}

TEST(OrcaNativeText, AFriendsRoomNamesItsPlayersOnTheirPlates)
{
  // No queue header: the plates of joined, named ports; the rules bar stays the game's.
  FakeMemory m = Css(0b0011);
  const std::vector<Orca::Events::PortInfo> ports{Port(0, "ada"), Port(1, "bo", -1, true),
                                                  Port(2, "cy")};
  u8 shown = 0;
  EXPECT_EQ(NT::Apply(m, ports, &shown), 2);
  EXPECT_EQ(shown, 0b0011);
  EXPECT_EQ(WordsOf(m, 0), "ADA");
  EXPECT_EQ(WordsOf(m, 1), "BO");
  EXPECT_EQ(WordsOf(m, 2), "  NONE");  // port 3 hasn't joined its panel
  EXPECT_EQ(WordsOf(m, RULES_BOX), "-man survival test!");
  // Idempotent: the same boundary again writes nothing.
  const int writes = m.writes;
  EXPECT_EQ(NT::Apply(m, ports, &shown), 0);
  EXPECT_EQ(m.writes, writes);
  EXPECT_EQ(shown, 0b0011);
  // The game formats a plate again (a tag picked, a port rejoining): the next boundary puts the
  // name back.
  PutBox(m, 0, Concat({kPlateSetup, Bytes("PLAYER 1")}));
  EXPECT_EQ(NT::Apply(m, ports, &shown), 1);
  EXPECT_EQ(WordsOf(m, 0), "ADA");
}

TEST(OrcaNativeText, NothingOffTheCharacterSelect)
{
  FakeMemory m = Css(0b0011);
  m.PutString(NAMES, "scSelStage");
  u8 shown = 0xFF;
  EXPECT_EQ(NT::Apply(m, {Port(0, "ada")}, &shown), 0);
  EXPECT_EQ(shown, 0);
}

TEST(OrcaNativeText, TheSameWordsOnBothMachines)
{
  // Ranked queue room, game 1, port 1 locked in: each machine sees the other as remote, and both
  // write the same bytes (no "you").
  const auto room = [](bool host) {
    FakeMemory m = Css(0b0011);
    PutHeader(m, MB::FLAG_QUEUE2, true);
    m.Write8(MB::PLUGGED, 3);
    m.Write8(Queue::READY, 1);
    m.Write16(Queue::TIMER, 6 * 60);
    std::vector<Orca::Events::PortInfo> ports{Port(0, "ada", 1532, !host),
                                              Port(1, "bo", 1490, host)};
    NT::Apply(m, ports);
    return m;
  };
  const FakeMemory a = room(true);
  const FakeMemory b = room(false);
  EXPECT_EQ(a.bytes, b.bytes);
  EXPECT_EQ(WordsOf(a, 0), "ADA 1532");
  EXPECT_EQ(WordsOf(a, 1), "BO 1490");
  EXPECT_EQ(WordsOf(a, RULES_BOX), "Game 1: ADA locked in, BO to press Start  0:24");
}

TEST(OrcaNativeText, TheRulesBarFollowsTheQueue)
{
  const std::vector<Orca::Events::PortInfo> ports{Port(0, "ada", 1532), Port(1, "bo", 1490, true)};
  const auto line = [&](auto setup) {
    FakeMemory m = Css(0b0011);
    PutHeader(m, MB::FLAG_QUEUE2, false);
    m.Write8(MB::PLUGGED, 3);
    setup(m);
    NT::Apply(m, ports);
    return WordsOf(m, RULES_BOX);
  };
  EXPECT_EQ(line([](FakeMemory&) {}), "Press Start to lock in  0:30");
  EXPECT_EQ(line([](FakeMemory& m) { m.Write8(Queue::READY, 2); }),
            "BO locked in, ADA to press Start  0:30");
  EXPECT_EQ(line([](FakeMemory& m) { m.Write8(Queue::READY, 3); }), "Both locked in");
  EXPECT_EQ(line([](FakeMemory& m) { m.Write8(Queue::FLAGS, Queue::FLAG_GO); }),
            "Both locked in, here we go!");
  EXPECT_EQ(line([](FakeMemory& m) {
              m.Write8(Queue::FLAGS, Queue::FLAG_TIMED_OUT);
              m.Write8(Queue::TIMEOUT_WHO, 2);
            }),
            "Time's up, BO didn't lock in");
  EXPECT_EQ(line([](FakeMemory& m) { m.Write8(MB::PLUGGED, 1); }),
            "Opponent found, waiting for BO...");
  // Casual's later games: a pick first, and no timer.
  EXPECT_EQ(line([](FakeMemory& m) { m.Write8(MB::FOUGHT, 1); }),
            "Pick with A, then Start to lock in");
  // In casual the plates carry no rating.
  FakeMemory m = Css(0b0011);
  PutHeader(m, MB::FLAG_QUEUE2, false);
  NT::Apply(m, ports);
  EXPECT_EQ(WordsOf(m, 0), "ADA");
}

TEST(OrcaNativeText, TheQueuesOwnCharacterSelect)
{
  FakeMemory m = Css(0b0001);
  PutHeader(m, MB::FLAG_QUEUE2 | MB::FLAG_SOLO, true);
  const std::vector<Orca::Events::PortInfo> ports{Port(0, "ada", 1532)};
  u8 shown = 0;
  NT::Apply(m, ports, &shown);
  EXPECT_EQ(WordsOf(m, RULES_BOX), "Ranked: pick, then press Start to search");
  // Solo: no rating on the plate (only a room's opponent needs it).
  EXPECT_EQ(WordsOf(m, 0), "ADA");
  EXPECT_EQ(shown, 0x01 | NT::SHOWN_RULES);
  m.Write8(Queue::READY, 1);
  NT::Apply(m, ports, &shown);
  EXPECT_EQ(WordsOf(m, RULES_BOX), "Ranked: searching for an opponent...  B to cancel");
}

TEST(OrcaNativeText, TheCharacterOrderAndTheSetInTheThirdPerson)
{
  // Ranked game 2 after ada won game 1: the character order, winner first.
  const auto room = [](CharOrder::Step step, int elapsed) {
    FakeMemory m = Css(0b0011);
    PutHeader(m, MB::FLAG_QUEUE2, true);
    m.Write8(MB::PLUGGED, 3);
    m.Write8(MB::FOUGHT, 1);
    m.Write8(MB::BASE + SetBlock::SET_GAMES, 1);
    m.Write8(MB::BASE + SetBlock::SET_WINS, 1);
    m.Write8(MB::BASE + SetBlock::SET_WINNER, SetBlock::NO_PORT);
    m.Write8(MB::BASE + SetBlock::SET_LAST_WINNER, 0);
    CharOrder::State st;
    st.game = 2;
    st.step = step;
    st.first = 0;
    st.elapsed = static_cast<u16>(elapsed);
    CharOrder::WriteState(m, st);
    return m;
  };
  const std::vector<Orca::Events::PortInfo> ports{Port(0, "ada", 1532), Port(1, "bo", 1490, true)};
  FakeMemory first = room(CharOrder::Step::First, 2 * 60);
  NT::Apply(first, ports);
  EXPECT_EQ(WordsOf(first, RULES_BOX), "ADA 1-0 BO: ADA picks, A then Start  0:43");
  FakeMemory second = room(CharOrder::Step::Second, 0);
  NT::Apply(second, ports);
  EXPECT_EQ(WordsOf(second, RULES_BOX), "ADA 1-0 BO: BO picks, A then Start  0:45");
  // The set over: who won it, nothing else.
  FakeMemory over = room(CharOrder::Step::Idle, 0);
  over.Write8(MB::BASE + SetBlock::SET_WINS, 2);
  over.Write8(MB::BASE + SetBlock::SET_DONE, 1);
  over.Write8(MB::BASE + SetBlock::SET_WINNER, 0);
  NT::Apply(over, ports);
  EXPECT_EQ(WordsOf(over, RULES_BOX), "ADA wins the set 2-0");
}
