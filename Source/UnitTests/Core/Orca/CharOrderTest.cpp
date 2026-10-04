// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// A ranked set's character order on later games (Core/Orca/UX/CharOrder.h): the step machine, the
// gate's masks and presses, the overlay's line, the state's bytes, the Meta Knight re-check and the
// test knob.

#include <map>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/UX/CharOrder.h"
#include "Core/Orca/UX/NameTags.h"
#include "InputCommon/GCPadStatus.h"

using namespace Orca::UX::CharOrder;
using Rollback::InputGate::ALL;
using Rollback::InputGate::Mask;

namespace
{
class FakeMemory final : public Orca::UX::GuestMemory
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
  std::map<u32, u8> bytes;
};

// The match block's last 16 bytes as the game leaves them: dead code, never the magic.
FakeMemory DeadArea()
{
  FakeMemory m;
  for (u32 i = 0; i < 16; ++i)
    m.bytes[kArea + i] = static_cast<u8>(0x3A + i);
  return m;
}

// Project+, game 2, port 1 won game 1, in the standard order (the winner first).
const SetView kGame2{Ruleset::PPlus, 2, 0, Order::WinnerFirst};
// Brawl, game 2, port 2 won game 1, in Luis's order (the loser asked first, the winner free).
const SetView kBrawl2{Ruleset::Brawl, 2, 1, Order::Free};

State St(int game, Step step, int first, u32 since, int elapsed, int locked = 0)
{
  return State{static_cast<u8>(game), step,  static_cast<u8>(first), static_cast<u8>(locked),
               since,                 static_cast<u16>(elapsed)};
}

CssView Css(bool placed0 = true, bool placed1 = true)
{
  CssView css;
  css.on_css = true;
  css.ports[0] = {true, 0x05, placed0, 0};
  css.ports[1] = {true, 0x11, placed1, 0};
  return css;
}

CssView Press(CssView css, int port, u16 buttons)
{
  css.ports[port].pressed = buttons;
  return css;
}

// Runs the machine as the hook does: every boundary twice (a first run and its re-run from the
// snapshot that holds the first run's write), which must agree.
State Twice(const SetView& set, const CssView& css, const State& state, int frame)
{
  const State once = Advance(set, css, state, frame);
  EXPECT_EQ(Advance(set, css, once, frame), once) << "not idempotent at frame " << frame;
  return once;
}
}  // namespace

TEST(OrcaCharOrder, StartsWithTheWinnerOnAProjectPlusLaterGame)
{
  const State s = Twice(kGame2, Css(), State{}, 1000);
  EXPECT_EQ(s, (St(2, Step::First, 0, 1000, 0)));
  // Port 2 won: port 2 first.
  const State t = Twice(SetView{Ruleset::PPlus, 3, 1, Order::WinnerFirst}, Css(), State{}, 50);
  EXPECT_EQ(t.step, Step::First);
  EXPECT_EQ(t.first, 1);
  EXPECT_EQ(t.game, 3);
}

TEST(OrcaCharOrder, NothingOnGameOneFriendsOrOffTheCharacterSelect)
{
  EXPECT_EQ(Twice(SetView{Ruleset::PPlus, 1, -1}, Css(), State{}, 10), State{});
  EXPECT_EQ(Twice(SetView{Ruleset::Brawl, 1, -1}, Css(), State{}, 10), State{});
  EXPECT_EQ(Twice(SetView{}, Css(), State{}, 10), State{});
  CssView one = Css();
  one.ports[1].human = false;  // only one player
  EXPECT_EQ(Twice(kGame2, one, State{}, 10), State{});
  CssView off;  // the stage select, the match
  const State on = St(2, Step::Second, 0, 5, 7);
  EXPECT_EQ(Twice(kGame2, off, on, 99), on);
  EXPECT_EQ(Gate(kGame2, off, on), Rollback::InputGate::Masks{});
}

TEST(OrcaCharOrder, StaleOrderClearsOnACharacterSelectItDoesNotApplyTo)
{
  const State stale = St(2, Step::Done, 0, 5, 7);
  // A new set's game 1, or a friends game, on the character select: cleared.
  EXPECT_EQ(Twice(SetView{Ruleset::PPlus, 1, -1}, Css(), stale, 10), State{});
  EXPECT_EQ(Twice(SetView{}, Css(), stale, 10), State{});
  // The next game of the same set starts over with its own winner.
  const State next = Twice(SetView{Ruleset::PPlus, 3, 1, Order::WinnerFirst}, Css(), stale, 10);
  EXPECT_EQ(next, (St(3, Step::First, 1, 10, 0)));
}

TEST(OrcaCharOrder, WinnerLocksInWithAThenTheLoser)
{
  State s = Twice(kGame2, Css(), State{}, 100);
  // A press already there on the first boundary (made before the order began) doesn't count.
  s = Twice(kGame2, Press(Css(), 0, PAD_BUTTON_A), s, 100);
  EXPECT_EQ(s.step, Step::First);
  // The loser's A does nothing on the winner's turn; nor does the winner's B or Start.
  s = Twice(kGame2, Press(Css(), 1, PAD_BUTTON_A), s, 101);
  s = Twice(kGame2, Press(Css(), 0, PAD_BUTTON_START | PAD_BUTTON_B), s, 102);
  EXPECT_EQ(s, (St(2, Step::First, 0, 100, 2)));
  // A with the token in the hand isn't a pick.
  s = Twice(kGame2, Press(Css(false, true), 0, PAD_BUTTON_A), s, 103);
  EXPECT_EQ(s.step, Step::First);
  // A with the token on a character locks it in.
  s = Twice(kGame2, Press(Css(), 0, PAD_BUTTON_A), s, 104);
  EXPECT_EQ(s, (St(2, Step::Second, 0, 104, 0)));
  // The same A, still in memory at the next boundary, isn't the loser's.
  s = Twice(kGame2, Press(Css(), 0, PAD_BUTTON_A), s, 105);
  EXPECT_EQ(s, (St(2, Step::Second, 0, 104, 1)));
  s = Twice(kGame2, Press(Css(), 1, PAD_BUTTON_A), s, 160);
  EXPECT_EQ(s, (St(2, Step::Done, 0, 160, 0)));
  s = Twice(kGame2, Css(), s, 170);
  EXPECT_EQ(s, (St(2, Step::Done, 0, 160, 10)));
}

TEST(OrcaCharOrder, EachPickTimesOutAfter45Seconds)
{
  State s = Twice(kGame2, Css(), State{}, 0);
  s = Twice(kGame2, Css(), s, PICK_FRAMES - 1);
  EXPECT_EQ(s.step, Step::First);
  EXPECT_EQ(s.elapsed, PICK_FRAMES - 1);
  s = Twice(kGame2, Css(), s, PICK_FRAMES);
  EXPECT_EQ(s, (St(2, Step::Second, 0, PICK_FRAMES, 0)));
  s = Twice(kGame2, Css(), s, 2 * PICK_FRAMES);
  EXPECT_EQ(s, (St(2, Step::Done, 0, 2 * PICK_FRAMES, 0)));
  // The time is up even with the token in the hand: the order goes on (the gate keeps B masked, so
  // a token is in a hand only if the player took it with A).
  State t = Twice(kGame2, Css(false, false), State{}, 0);
  t = Twice(kGame2, Css(false, false), t, PICK_FRAMES);
  EXPECT_EQ(t.step, Step::Second);
}

TEST(OrcaCharOrder, GateMasksWhoseTurnItIsNot)
{
  const CssView css = Css();
  const auto first = Gate(kGame2, css, St(2, Step::First, 0, 0, 3));
  EXPECT_EQ(first[1], ALL);
  EXPECT_EQ(first[0], (Mask{PAD_BUTTON_B | PAD_BUTTON_START, false, false, 0}));
  EXPECT_TRUE(first[2].Empty() && first[3].Empty());
  const auto second = Gate(kGame2, css, St(2, Step::Second, 0, 0, 3));
  EXPECT_EQ(second[0], ALL);
  EXPECT_EQ(second[1], (Mask{PAD_BUTTON_B, false, false, 0}));
  // Port 2 won: the other way round.
  const SetView p2won{Ruleset::PPlus, 2, 1, Order::WinnerFirst};
  const auto first2 = Gate(p2won, css, St(2, Step::First, 1, 0, 3));
  EXPECT_EQ(first2[0], ALL);
  EXPECT_EQ(first2[1].buttons, PAD_BUTTON_B | PAD_BUTTON_START);
}

TEST(OrcaCharOrder, DonePressesStartForTheLoserEveryOtherFrame)
{
  const CssView css = Css();
  const auto even = Gate(kGame2, css, St(2, Step::Done, 0, 0, 4));
  EXPECT_EQ(even[0], ALL);
  EXPECT_EQ(even[1].buttons, ALL.buttons);
  EXPECT_EQ(even[1].press, PAD_BUTTON_START);
  const auto odd = Gate(kGame2, css, St(2, Step::Done, 0, 0, 5));
  EXPECT_EQ(odd[1].press, 0);
  // What the game reads: the loser's own Start is gone, the synthesized one is there.
  GCPadStatus pad{};
  pad.isConnected = true;
  pad.button = PAD_BUTTON_A;
  const GCPadStatus out = Rollback::InputGate::Apply(pad, even[1]);
  EXPECT_EQ(out.button, PAD_BUTTON_START);
}

TEST(OrcaCharOrder, GateIgnoresAStateTheMachineWouldClear)
{
  // A state from another game, another winner, or with no set: no masks.
  EXPECT_EQ(Gate(SetView{Ruleset::PPlus, 3, 0, Order::WinnerFirst}, Css(),
                 St(2, Step::First, 0, 0, 0)),
            Rollback::InputGate::Masks{});
  EXPECT_EQ(Gate(SetView{Ruleset::PPlus, 2, 1, Order::WinnerFirst}, Css(),
                 St(2, Step::First, 0, 0, 0)),
            Rollback::InputGate::Masks{});
  // A state of one order read under the other: its first picker isn't the order's.
  EXPECT_EQ(Gate(SetView{Ruleset::PPlus, 2, 0, Order::Free}, Css(), St(2, Step::First, 0, 0, 0)),
            Rollback::InputGate::Masks{});
  EXPECT_EQ(Gate(SetView{}, Css(), St(2, Step::First, 0, 0, 0)), Rollback::InputGate::Masks{});
  EXPECT_EQ(Gate(kGame2, Css(), State{}), Rollback::InputGate::Masks{});
}

TEST(OrcaCharOrder, LineSaysWhoseTurnAndTheTimeLeft)
{
  std::vector<Orca::Events::PortInfo> ports{{0, "ADA", false, {}}, {1, "BO", true, {}}};
  EXPECT_EQ(Line(kGame2, Css(), St(2, Step::First, 0, 0, 0), ports),
            "You pick first · 0:45 · A locks it in");
  EXPECT_EQ(Line(kGame2, Css(), St(2, Step::Second, 0, 0, 61), ports), "BO picks · 0:44");
  ports[0].remote = true;
  ports[1].remote = false;
  EXPECT_EQ(Line(kGame2, Css(), St(2, Step::First, 0, 0, 2700 - 59), ports),
            "ADA picks first · 0:01");
  EXPECT_EQ(Line(kGame2, Css(), St(2, Step::Second, 0, 0, 0), ports),
            "Your pick · 0:45 · A locks it in");
  EXPECT_EQ(Line(kGame2, Css(), St(2, Step::Done, 0, 0, 0), ports), "");
  EXPECT_EQ(Line(SetView{}, Css(), St(2, Step::First, 0, 0, 0), ports), "");
  // No names: the port.
  EXPECT_EQ(Line(kGame2, Css(), St(2, Step::Second, 0, 0, 0), {}), "P2 picks · 0:45");
}

TEST(OrcaCharOrder, StateBytesRoundTripAndWriteOnlyWhatDiffers)
{
  FakeMemory m = DeadArea();
  EXPECT_EQ(ReadState(m), State{});  // dead code: no magic
  const State s = St(2, Step::Second, 1, 0x01020304, 0x0506, 2);
  EXPECT_EQ(WriteState(m, s), 16);
  EXPECT_EQ(ReadState(m), s);
  EXPECT_EQ(WriteState(m, s), 0);
  State t = s;
  t.elapsed = 0x0507;
  EXPECT_EQ(WriteState(m, t), 1);
  EXPECT_EQ(ReadState(m), t);
  EXPECT_EQ(m.Read32(kArea), kMagic);
  // A step byte out of range reads as nothing.
  m.Write8(kArea + 5, 9);
  EXPECT_EQ(ReadState(m), State{});
}

TEST(OrcaCharOrder, MetaKnightClause)
{
  constexpr int MK = 0x18;
  EXPECT_EQ(MkFreePicker({MK, 0x00}, MK), 1);
  EXPECT_EQ(MkFreePicker({0x10, MK}, MK), 0);
  EXPECT_EQ(MkFreePicker({MK, MK}, MK), -1);
  EXPECT_EQ(MkFreePicker({0x00, 0x10}, MK), -1);
}

TEST(OrcaCharOrder, TestKnob)
{
  SetView set;
  ASSERT_TRUE(ParseTestSpec("pplus:2:1", &set));
  EXPECT_EQ(set, (SetView{Ruleset::PPlus, 2, 0, kLaterGameOrder}));
  ASSERT_TRUE(ParseTestSpec("brawl:3:2", &set));
  EXPECT_EQ(set, (SetView{Ruleset::Brawl, 3, 1, kLaterGameOrder}));
  ASSERT_TRUE(ParseTestSpec("pplus:2:1:winnerfirst", &set));
  EXPECT_EQ(set, (SetView{Ruleset::PPlus, 2, 0, Order::WinnerFirst}));
  ASSERT_TRUE(ParseTestSpec("brawl:2:2:free", &set));
  EXPECT_EQ(set, (SetView{Ruleset::Brawl, 2, 1, Order::Free}));
  for (const char* bad : {"", "pplus", "pplus:2", "pplus:2:3", "pplus:0:1", "melee:2:1",
                          "pplus:x:1", "pplus:2:1:", "pplus:200:1", "pplus:2:1:loser",
                          "pplus:2:1:free:1"})
  {
    EXPECT_FALSE(ParseTestSpec(bad, &set)) << bad;
  }
}

TEST(OrcaCharOrder, TheOrderIsOneConstantFreeByDefault)
{
  // Luis's order unless the integrator flips kLaterGameOrder; the set read from the match block
  // carries it.
  EXPECT_EQ(kLaterGameOrder, Order::Free);
  EXPECT_EQ(SetView{}.order, kLaterGameOrder);
  EXPECT_EQ(FirstPicker(SetView{Ruleset::PPlus, 2, 0, Order::Free}), 1);
  EXPECT_EQ(FirstPicker(SetView{Ruleset::PPlus, 2, 0, Order::WinnerFirst}), 0);
  EXPECT_EQ(FirstPicker(SetView{Ruleset::Brawl, 2, 1, Order::Free}), 0);
  EXPECT_EQ(FirstPicker(SetView{Ruleset::Brawl, 2, 1, Order::WinnerFirst}), 1);
  EXPECT_EQ(FirstPicker(SetView{Ruleset::Brawl, 1, -1, Order::Free}), -1);
}

TEST(OrcaCharOrder, FreeAsksTheLoserFirstAndTheWinnerMayLockInAnyTime)
{
  // Port 2 won game 1: port 1 (the loser) is asked first.
  State s = Twice(kBrawl2, Css(), State{}, 10);
  EXPECT_EQ(s, St(2, Step::First, 0, 10, 0));
  const auto first = Gate(kBrawl2, Css(), s);
  EXPECT_EQ(first[0], (Mask{PAD_BUTTON_B | PAD_BUTTON_START, false, false, 0}));
  // The winner isn't kept waiting: it can lock in (no B, no Start).
  EXPECT_EQ(first[1], (Mask{PAD_BUTTON_B | PAD_BUTTON_START, false, false, 0}));
  // The winner locks in at once (a one-character player); the loser's turn goes on.
  s = Twice(kBrawl2, Press(Css(), 1, PAD_BUTTON_A), s, 20);
  EXPECT_EQ(s, St(2, Step::First, 0, 10, 10, 2));
  const auto waiting = Gate(kBrawl2, Css(), s);
  EXPECT_EQ(waiting[1], ALL);
  EXPECT_EQ(waiting[0].buttons, PAD_BUTTON_B | PAD_BUTTON_START);
  // A with the token in the hand isn't a lock-in.
  s = Twice(kBrawl2, Press(Css(false, true), 0, PAD_BUTTON_A), s, 21);
  EXPECT_EQ(s.step, Step::First);
  // The loser locks in: the winner already has, so straight to Done.
  s = Twice(kBrawl2, Press(Css(), 0, PAD_BUTTON_A), s, 30);
  EXPECT_EQ(s, St(2, Step::Done, 0, 30, 0, 2));
  // Done: Start for the second picker (the winner, port 2).
  const auto done = Gate(kBrawl2, Css(), s);
  EXPECT_EQ(done[1].press, PAD_BUTTON_START);
  EXPECT_EQ(done[0].press, 0);
}

TEST(OrcaCharOrder, FreeWinnerWhoWaitsGetsItsOwnTurn)
{
  const SetView set{Ruleset::PPlus, 2, 0, Order::Free};  // port 1 won: port 2 asked first
  State s = Twice(set, Css(), State{}, 100);
  EXPECT_EQ(s, St(2, Step::First, 1, 100, 0));
  s = Twice(set, Press(Css(), 1, PAD_BUTTON_A), s, 150);
  // The loser locked in, the winner hadn't: the winner's turn, its own 45 s.
  EXPECT_EQ(s, St(2, Step::Second, 1, 150, 0));
  const auto second = Gate(set, Css(), s);
  EXPECT_EQ(second[1], ALL);
  EXPECT_EQ(second[0], (Mask{PAD_BUTTON_B, false, false, 0}));
  s = Twice(set, Css(), s, 150 + PICK_FRAMES);
  EXPECT_EQ(s, St(2, Step::Done, 1, 150 + PICK_FRAMES, 0));
  // The loser's time running out with the winner locked in: Done too.
  State t = Twice(set, Css(), State{}, 0);
  t = Twice(set, Press(Css(), 0, PAD_BUTTON_A), t, 5);
  t = Twice(set, Css(), t, PICK_FRAMES);
  EXPECT_EQ(t, St(2, Step::Done, 1, PICK_FRAMES, 0, 1));
}

TEST(OrcaCharOrder, FreeLine)
{
  // Port 2 won: port 1 (ADA) is asked first.
  std::vector<Orca::Events::PortInfo> ports{{0, "ADA", false, {}}, {1, "BO", true, {}}};
  EXPECT_EQ(Line(kBrawl2, Css(), St(2, Step::First, 0, 0, 0), ports),
            "You pick first · 0:45 · A locks it in");
  ports[0].remote = true;
  ports[1].remote = false;
  EXPECT_EQ(Line(kBrawl2, Css(), St(2, Step::First, 0, 0, 600), ports),
            "ADA picks first · 0:35 · A locks yours in any time");
  EXPECT_EQ(Line(kBrawl2, Css(), St(2, Step::First, 0, 0, 600, 2), ports),
            "Waiting for ADA · 0:35");
  EXPECT_EQ(Line(kBrawl2, Css(), St(2, Step::Second, 0, 0, 0), ports),
            "Your pick · 0:45 · A locks it in");
  EXPECT_EQ(Line(kBrawl2, Css(), St(2, Step::First, 0, 0, 0), {}), "P1 picks first · 0:45");
}

TEST(OrcaCharOrder, ReadsTheSetFromTheMatchBlock)
{
  FakeMemory m;
  const u32 base = Orca::UX::FreeSpace::kMatchBlock.begin;
  for (u32 i = 0; i < 0x290; ++i)
    m.bytes[base + i] = 0;
  EXPECT_EQ(ReadSet(m), SetView{});  // no magic: friends, solo
  m.Write32(base + Block::MAGIC, Block::MAGIC_VALUE);
  m.Write8(base + Block::VERSION, 1);
  m.Write8(base + Block::MODE, Block::MODE_RANKED);
  m.Write8(base + Block::RULESET, 2);
  m.Write8(base + Block::SET_LAST_WINNER, 0xFF);
  EXPECT_EQ(ReadSet(m), (SetView{Ruleset::PPlus, 1, -1}));
  // Port 2 won game 1.
  m.Write8(base + Block::SET_WINS + 1, 1);
  m.Write8(base + Block::SET_LAST_WINNER, 1);
  EXPECT_EQ(ReadSet(m), (SetView{Ruleset::PPlus, 2, 1}));
  m.Write8(base + Block::RULESET, 1);
  m.Write8(base + Block::SET_WINS, 1);
  m.Write8(base + Block::SET_LAST_WINNER, 0);
  EXPECT_EQ(ReadSet(m), (SetView{Ruleset::Brawl, 3, 0}));
  // Casual, a decided set, an unknown ruleset or version: no set.
  m.Write8(base + Block::MODE, 1);
  EXPECT_EQ(ReadSet(m), SetView{});
  m.Write8(base + Block::MODE, Block::MODE_RANKED);
  m.Write8(base + Block::SET_DONE, 1);
  EXPECT_EQ(ReadSet(m), SetView{});
  m.Write8(base + Block::SET_DONE, 0);
  m.Write8(base + Block::RULESET, 7);
  EXPECT_EQ(ReadSet(m), SetView{});
  m.Write8(base + Block::RULESET, 1);
  m.Write8(base + Block::VERSION, 2);
  EXPECT_EQ(ReadSet(m), SetView{});
}

TEST(OrcaCharOrder, DoneWithATokenStillInAHand)
{
  // Port 1 (the winner) picked its token up with A and its time ran out, the hand over a
  // character: it can move and put it down, Orca puts it there every other frame, and no Start yet.
  CssView css = Css(false, true);
  const auto even = Gate(kGame2, css, St(2, Step::Done, 0, 0, 4));
  EXPECT_EQ(even[0].buttons, ALL.buttons & ~PAD_BUTTON_A);
  EXPECT_FALSE(even[0].main_stick);
  EXPECT_EQ(even[0].press, PAD_BUTTON_A);
  EXPECT_EQ(even[1], ALL);
  EXPECT_EQ(Gate(kGame2, css, St(2, Step::Done, 0, 0, 5))[0].press, 0);
  // Over no character: nothing pressed for it.
  css.ports[0].character = kNoCharacter;
  EXPECT_EQ(Gate(kGame2, css, St(2, Step::Done, 0, 0, 4))[0].press, 0);
  std::vector<Orca::Events::PortInfo> ports{{0, "ADA", true, {}}, {1, "BO", false, {}}};
  EXPECT_EQ(Line(kGame2, css, St(2, Step::Done, 0, 0, 4), ports),
            "Waiting for ADA to put the token down");
  ports[0].remote = false;
  ports[1].remote = true;
  EXPECT_EQ(Line(kGame2, css, St(2, Step::Done, 0, 0, 4), ports), "Put your token on a character");
}

TEST(OrcaCharOrder, ReadsTheCharacterSelect)
{
  FakeMemory m;
  // The scene manager, the scene named scSelctCharacter, its task and two player areas.
  const auto zero = [&m](u32 a, u32 n) {
    for (u32 i = 0; i < n; ++i)
      m.bytes[a + i] = 0;
  };
  zero(0x805A0060, 4);
  zero(0x80900000, 8);
  zero(0x80910000, 0x404);
  zero(0x80930000, 0x50);
  zero(0x80940000, 0x200);
  zero(0x80950000, 0x200);
  m.Write32(0x805A0060, 0x80900000);
  m.Write32(0x80900004, 0x80910000);
  m.Write32(0x80910000, 0x80920000);
  const char* name = "scSelctCharacter";
  for (u32 i = 0; i <= 16; ++i)
    m.bytes[0x80920000 + i] = i < 16 ? static_cast<u8>(name[i]) : 0;
  m.Write32(0x80910400, 0x80930000);
  m.Write32(0x80930044, 0x80940000);
  m.Write32(0x80930048, 0x80950000);
  m.Write32(0x80940000 + 0x1B4, 1);
  m.Write32(0x80940000 + 0x1B8, 0x16);
  m.Write8(0x80940000 + 0x1F8, 0);
  m.Write32(0x80940000 + 0x10, PAD_BUTTON_A);
  m.Write32(0x80950000 + 0x1B4, 1);
  m.Write32(0x80950000 + 0x1B8, 0x28);
  m.Write8(0x80950000 + 0x1F8, 1);
  const CssView css = ReadCss(m);
  EXPECT_TRUE(css.on_css);
  EXPECT_EQ(css.ports[0], (CssPort{true, 0x16, true, PAD_BUTTON_A}));
  EXPECT_EQ(css.ports[1], (CssPort{true, kNoCharacter, false, 0}));
  // Another scene: not the character select.
  m.bytes[0x80920000 + 2] = 'X';
  EXPECT_FALSE(ReadCss(m).on_css);
}

TEST(OrcaCharOrder, OrderHoldsWhileAPlayerIsNotAHuman)
{
  // Once begun, the player-type button can't start the order over or lift its masks.
  State s = Twice(kGame2, Css(), State{}, 10);
  CssView cpu = Css();
  cpu.ports[1].human = false;
  s = Twice(kGame2, cpu, s, 20);
  EXPECT_EQ(s, St(2, Step::First, 0, 10, 10));
  EXPECT_EQ(Gate(kGame2, cpu, s)[1], ALL);
  s = Twice(kGame2, Css(), s, 30);
  EXPECT_EQ(s, St(2, Step::First, 0, 10, 20));
}
