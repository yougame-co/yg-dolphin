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
FakeMemory Block()
{
  FakeMemory m;
  for (u32 i = 0; i < 16; ++i)
    m.bytes[kArea + i] = static_cast<u8>(0x3A + i);
  return m;
}

const SetView kGame2{Ruleset::PPlus, 2, 0};  // Project+, game 2, port 1 won game 1

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
State Run(const SetView& set, const CssView& css, const State& state, int frame)
{
  const State once = Advance(set, css, state, frame);
  EXPECT_EQ(Advance(set, css, once, frame), once) << "not idempotent at frame " << frame;
  return once;
}
}  // namespace

TEST(OrcaCharOrder, StartsWithTheWinnerOnAProjectPlusLaterGame)
{
  const State s = Run(kGame2, Css(), State{}, 1000);
  EXPECT_EQ(s, (State{2, Step::First, 0, 1000, 0}));
  // Port 2 won: port 2 first.
  const State t = Run(SetView{Ruleset::PPlus, 3, 1}, Css(), State{}, 50);
  EXPECT_EQ(t.step, Step::First);
  EXPECT_EQ(t.first, 1);
  EXPECT_EQ(t.game, 3);
}

TEST(OrcaCharOrder, NothingOnGameOneBrawlFriendsOrOffTheCharacterSelect)
{
  EXPECT_EQ(Run(SetView{Ruleset::PPlus, 1, -1}, Css(), State{}, 10), State{});
  EXPECT_EQ(Run(SetView{Ruleset::Brawl, 2, 0}, Css(), State{}, 10), State{});
  EXPECT_EQ(Run(SetView{}, Css(), State{}, 10), State{});
  CssView one = Css();
  one.ports[1].human = false;  // only one player
  EXPECT_EQ(Run(kGame2, one, State{}, 10), State{});
  CssView off;  // the stage select, the match
  const State on{2, Step::Second, 0, 5, 7};
  EXPECT_EQ(Run(kGame2, off, on, 99), on);
  EXPECT_EQ(Gate(kGame2, off, on), Rollback::InputGate::Masks{});
}

TEST(OrcaCharOrder, StaleOrderClearsOnACharacterSelectItDoesNotApplyTo)
{
  const State stale{2, Step::Done, 0, 5, 7};
  // A new set's game 1, or a friends game, on the character select: cleared.
  EXPECT_EQ(Run(SetView{Ruleset::PPlus, 1, -1}, Css(), stale, 10), State{});
  EXPECT_EQ(Run(SetView{}, Css(), stale, 10), State{});
  // The next game of the same set starts over with its own winner.
  const State next = Run(SetView{Ruleset::PPlus, 3, 1}, Css(), stale, 10);
  EXPECT_EQ(next, (State{3, Step::First, 1, 10, 0}));
}

TEST(OrcaCharOrder, WinnerLocksInWithAThenTheLoser)
{
  State s = Run(kGame2, Css(), State{}, 100);
  // A press already there on the first boundary (made before the order began) doesn't count.
  s = Run(kGame2, Press(Css(), 0, PAD_BUTTON_A), s, 100);
  EXPECT_EQ(s.step, Step::First);
  // The loser's A does nothing on the winner's turn; nor does the winner's B or Start.
  s = Run(kGame2, Press(Css(), 1, PAD_BUTTON_A), s, 101);
  s = Run(kGame2, Press(Css(), 0, PAD_BUTTON_START | PAD_BUTTON_B), s, 102);
  EXPECT_EQ(s, (State{2, Step::First, 0, 100, 2}));
  // A with the token in the hand isn't a pick.
  s = Run(kGame2, Press(Css(false, true), 0, PAD_BUTTON_A), s, 103);
  EXPECT_EQ(s.step, Step::First);
  // A with the token on a character locks it in.
  s = Run(kGame2, Press(Css(), 0, PAD_BUTTON_A), s, 104);
  EXPECT_EQ(s, (State{2, Step::Second, 0, 104, 0}));
  // The same A, still in memory at the next boundary, isn't the loser's.
  s = Run(kGame2, Press(Css(), 0, PAD_BUTTON_A), s, 105);
  EXPECT_EQ(s, (State{2, Step::Second, 0, 104, 1}));
  s = Run(kGame2, Press(Css(), 1, PAD_BUTTON_A), s, 160);
  EXPECT_EQ(s, (State{2, Step::Done, 0, 160, 0}));
  s = Run(kGame2, Css(), s, 170);
  EXPECT_EQ(s, (State{2, Step::Done, 0, 160, 10}));
}

TEST(OrcaCharOrder, EachPickTimesOutAfter45Seconds)
{
  State s = Run(kGame2, Css(), State{}, 0);
  s = Run(kGame2, Css(), s, PICK_FRAMES - 1);
  EXPECT_EQ(s.step, Step::First);
  EXPECT_EQ(s.elapsed, PICK_FRAMES - 1);
  s = Run(kGame2, Css(), s, PICK_FRAMES);
  EXPECT_EQ(s, (State{2, Step::Second, 0, PICK_FRAMES, 0}));
  s = Run(kGame2, Css(), s, 2 * PICK_FRAMES);
  EXPECT_EQ(s, (State{2, Step::Done, 0, 2 * PICK_FRAMES, 0}));
  // The time is up even with the token in the hand: the order goes on (the gate keeps B masked, so
  // a token is in a hand only if the player took it with A).
  State t = Run(kGame2, Css(false, false), State{}, 0);
  t = Run(kGame2, Css(false, false), t, PICK_FRAMES);
  EXPECT_EQ(t.step, Step::Second);
}

TEST(OrcaCharOrder, GateMasksWhoseTurnItIsNot)
{
  const CssView css = Css();
  const auto first = Gate(kGame2, css, State{2, Step::First, 0, 0, 3});
  EXPECT_EQ(first[1], ALL);
  EXPECT_EQ(first[0], (Mask{PAD_BUTTON_B | PAD_BUTTON_START, false, false, 0}));
  EXPECT_TRUE(first[2].Empty() && first[3].Empty());
  const auto second = Gate(kGame2, css, State{2, Step::Second, 0, 0, 3});
  EXPECT_EQ(second[0], ALL);
  EXPECT_EQ(second[1], (Mask{PAD_BUTTON_B, false, false, 0}));
  // Port 2 won: the other way round.
  const SetView p2won{Ruleset::PPlus, 2, 1};
  const auto first2 = Gate(p2won, css, State{2, Step::First, 1, 0, 3});
  EXPECT_EQ(first2[0], ALL);
  EXPECT_EQ(first2[1].buttons, PAD_BUTTON_B | PAD_BUTTON_START);
}

TEST(OrcaCharOrder, DonePressesStartForTheLoserEveryOtherFrame)
{
  const CssView css = Css();
  const auto even = Gate(kGame2, css, State{2, Step::Done, 0, 0, 4});
  EXPECT_EQ(even[0], ALL);
  EXPECT_EQ(even[1].buttons, ALL.buttons);
  EXPECT_EQ(even[1].press, PAD_BUTTON_START);
  const auto odd = Gate(kGame2, css, State{2, Step::Done, 0, 0, 5});
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
  EXPECT_EQ(Gate(SetView{Ruleset::PPlus, 3, 0}, Css(), State{2, Step::First, 0, 0, 0}),
            Rollback::InputGate::Masks{});
  EXPECT_EQ(Gate(SetView{Ruleset::PPlus, 2, 1}, Css(), State{2, Step::First, 0, 0, 0}),
            Rollback::InputGate::Masks{});
  EXPECT_EQ(Gate(SetView{}, Css(), State{2, Step::First, 0, 0, 0}), Rollback::InputGate::Masks{});
  EXPECT_EQ(Gate(kGame2, Css(), State{}), Rollback::InputGate::Masks{});
}

TEST(OrcaCharOrder, LineSaysWhoseTurnAndTheTimeLeft)
{
  std::vector<Orca::Events::PortInfo> ports{{0, "ADA", false, {}}, {1, "BO", true, {}}};
  EXPECT_EQ(Line(kGame2, Css(), State{2, Step::First, 0, 0, 0}, ports),
            "You pick first · 0:45 · A locks it in");
  EXPECT_EQ(Line(kGame2, Css(), State{2, Step::Second, 0, 0, 61}, ports), "BO picks · 0:44");
  ports[0].remote = true;
  ports[1].remote = false;
  EXPECT_EQ(Line(kGame2, Css(), State{2, Step::First, 0, 0, 2700 - 59}, ports),
            "ADA picks first · 0:01");
  EXPECT_EQ(Line(kGame2, Css(), State{2, Step::Second, 0, 0, 0}, ports),
            "Your pick · 0:45 · A locks it in");
  EXPECT_EQ(Line(kGame2, Css(), State{2, Step::Done, 0, 0, 0}, ports), "");
  EXPECT_EQ(Line(SetView{}, Css(), State{2, Step::First, 0, 0, 0}, ports), "");
  // No names: the port.
  EXPECT_EQ(Line(kGame2, Css(), State{2, Step::Second, 0, 0, 0}, {}), "P2 picks · 0:45");
}

TEST(OrcaCharOrder, StateBytesRoundTripAndWriteOnlyWhatDiffers)
{
  FakeMemory m = Block();
  EXPECT_EQ(ReadState(m), State{});  // dead code: no magic
  const State s{2, Step::Second, 1, 0x01020304, 0x0506};
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
  EXPECT_EQ(MkFreePicker({kMetaKnight, 0x00}), 1);
  EXPECT_EQ(MkFreePicker({0x10, kMetaKnight}), 0);
  EXPECT_EQ(MkFreePicker({kMetaKnight, kMetaKnight}), -1);
  EXPECT_EQ(MkFreePicker({0x00, 0x10}), -1);
}

TEST(OrcaCharOrder, TestKnob)
{
  SetView set;
  ASSERT_TRUE(ParseTestSpec("pplus:2:1", &set));
  EXPECT_EQ(set, (SetView{Ruleset::PPlus, 2, 0}));
  ASSERT_TRUE(ParseTestSpec("brawl:3:2", &set));
  EXPECT_EQ(set, (SetView{Ruleset::Brawl, 3, 1}));
  for (const char* bad : {"", "pplus", "pplus:2", "pplus:2:3", "pplus:0:1", "melee:2:1",
                          "pplus:x:1", "pplus:2:1:", "pplus:200:1"})
  {
    EXPECT_FALSE(ParseTestSpec(bad, &set)) << bad;
  }
}
