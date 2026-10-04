// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The queue's character select (Core/Orca/UX/Queue.h): ready, unready, the ready timer and its
// timeouts, game 1's locks, the joiner's pick steered in, the skip's hold, the raw latch.

#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/UX/MatchBlock.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/OnlineRules.h"
#include "Core/Orca/UX/Queue.h"
#include "InputCommon/GCPadStatus.h"

using namespace Orca::UX;
using namespace Orca::UX::Queue;
namespace MB = Orca::UX::MatchBlock;
using Rollback::InputGate::ALL;
using Rollback::InputGate::Masks;

namespace
{
// The stick's centre, as values (the header's constants have no definition to bind to).
constexpr int CX = GCPadStatus::MAIN_STICK_CENTER_X;
constexpr int CY = GCPadStatus::MAIN_STICK_CENTER_Y;
constexpr u16 START = PAD_BUTTON_START, B = PAD_BUTTON_B, A = PAD_BUTTON_A, X = PAD_BUTTON_X,
              Y = PAD_BUTTON_Y, Z = PAD_TRIGGER_Z;

CssPort Placed(int character, int costume = 0)
{
  CssPort p;
  p.readable = true;
  p.human = true;
  p.character = character;
  p.costume = costume;
  p.placed = true;
  p.hand_target = Rules::CSS_HAND_GRID;
  return p;
}

View Solo()
{
  View v;
  v.queue2 = true;
  v.solo = true;
  v.css = true;
  v.plugged = 1;
  v.ports[0] = Placed(0x05, 2);
  return v;
}

View Room(bool ranked = false)
{
  View v;
  v.queue2 = true;
  v.css = true;
  v.ranked = ranked;
  v.plugged = 3;
  v.ports[0] = Placed(0x05, 2);
  v.ports[1] = Placed(0x10, 1);
  return v;
}

// One boundary with these newly pressed buttons (the latch's raw view: the frame before had none).
View Press(View v, u16 port1, u16 port2 = 0)
{
  v.raw = {port1, port2};
  v.raw_prev = {0, 0};
  return v;
}

// The state after `view` at `frame`, checking that the boundary run again changes nothing.
State Step(const View& view, const State& state, int frame)
{
  const State after = Advance(view, state, frame);
  EXPECT_EQ(Advance(view, after, frame), after) << "frame " << frame << " isn't idempotent";
  return after;
}

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
  std::map<u32, u8> bytes;
};

FakeMemory Block(u8 flags)
{
  FakeMemory m;
  for (u32 i = 0; i < MB::FULL_SIZE; ++i)
    m.bytes[MB::BASE + i] = 0;
  m.Write32(MB::MAGIC, MB::MAGIC_VALUE);
  m.Write8(MB::VERSION, MB::VERSION_VALUE);
  m.Write8(MB::MODE, MB::MODE_CASUAL);
  m.Write8(MB::RULESET, 1);
  m.Write8(MB::FLAGS, flags);
  return m;
}

GCPadStatus Pad(u16 buttons)
{
  GCPadStatus pad;
  pad.isConnected = true;
  pad.button = buttons;
  return pad;
}
}  // namespace

TEST(OrcaQueue, TheIdentityRoundTrips)
{
  Identity id;
  id.rating = 1532;
  id.character = 0x12;
  id.costume = 3;
  id.x = -7.25f;
  id.y = 11.5f;
  const std::vector<u8> bytes = EncodeIdentity(id);
  EXPECT_EQ(bytes.size(), 13u);
  EXPECT_EQ(DecodeIdentity(bytes), id);
  // No rating, no pick.
  const std::vector<u8> none = EncodeIdentity(Identity{});
  ASSERT_TRUE(DecodeIdentity(none).has_value());
  EXPECT_EQ(DecodeIdentity(none)->rating, -1);
  EXPECT_FALSE(DecodeIdentity(none)->HasPick());
  // Too short, another version: none. A place no game holds: no pick (the player picks by hand).
  EXPECT_FALSE(DecodeIdentity({1, 2, 3}).has_value());
  std::vector<u8> v2 = bytes;
  v2[0] = 2;
  EXPECT_FALSE(DecodeIdentity(v2).has_value());
  id.x = std::nanf("");
  EXPECT_FALSE(DecodeIdentity(EncodeIdentity(id))->HasPick());
  EXPECT_EQ(DecodeIdentity(EncodeIdentity(id))->rating, 1532);
}

TEST(OrcaQueue, OnItsOwnCharacterSelectStartReadiesAndBUnreadies)
{
  State s;
  // Start without a character down: nothing.
  View v = Solo();
  v.ports[0].placed = false;
  s = Step(Press(v, START), s, 100);
  EXPECT_EQ(s.ready, 0);
  // With one: ready, and it stays while nothing new is pressed.
  s = Step(Press(Solo(), START), s, 101);
  EXPECT_EQ(s.ready, 1);
  s = Step(Solo(), s, 102);
  EXPECT_EQ(s.ready, 1);
  // B: not ready; Start and B together: B wins.
  s = Step(Press(Solo(), B), s, 103);
  EXPECT_EQ(s.ready, 0);
  s = Step(Press(Solo(), START | B), s, 104);
  EXPECT_EQ(s.ready, 0);
  // Taking the token up (another pick): not ready.
  s = Step(Press(Solo(), START), s, 105);
  EXPECT_EQ(s.ready, 1);
  v = Solo();
  v.ports[0].placed = false;
  s = Step(v, s, 106);
  EXPECT_EQ(s.ready, 0);
  // A held Start isn't a new press.
  View held = Solo();
  held.raw = {START, 0};
  held.raw_prev = {START, 0};
  s = Step(held, s, 107);
  EXPECT_EQ(s.ready, 0);
  // No timer, no go on the queue's own character select.
  s = Step(Press(Solo(), START), s, 108 + READY_FRAMES * 2);
  EXPECT_EQ(s.flags, 0);
  EXPECT_EQ(s.timer_start, 0u);
}

TEST(OrcaQueue, TheBThatStopsTheSearchIsSwallowedUntilLetGo)
{
  State s = Step(Press(Solo(), START), State{}, 400);
  ASSERT_EQ(s.ready, 1);
  // B: not ready, and B stays from the game while it is held (the token stays down).
  s = Step(Press(Solo(), B), s, 401);
  EXPECT_EQ(s.ready, 0);
  EXPECT_TRUE(s.flags & FLAG_SWALLOW_B);
  View held = Solo();
  held.raw = {B, 0};
  held.raw_prev = {B, 0};
  s = Step(held, s, 402);
  EXPECT_TRUE(s.flags & FLAG_SWALLOW_B);
  EXPECT_TRUE(Gate(held, s)[0].buttons & B);
  // Let go: the game has B again (held B backs out to the menus).
  View free = Solo();
  free.raw_prev = {B, 0};
  s = Step(free, s, 403);
  EXPECT_FALSE(s.flags & FLAG_SWALLOW_B);
  EXPECT_FALSE(Gate(free, s)[0].buttons & B);
  // A B pressed while not ready is the game's from the first frame.
  s = Step(Press(Solo(), B), s, 404);
  EXPECT_FALSE(s.flags & FLAG_SWALLOW_B);
  EXPECT_FALSE(Gate(Solo(), s)[0].buttons & B);
}

TEST(OrcaQueue, ItsOwnCharacterSelectsMasks)
{
  State s;
  Masks m = Gate(Solo(), s);
  // Start is Orca's, Z the room's skip; B backs out while not ready; nobody else plays.
  EXPECT_EQ(m[0].buttons, START | Z);
  EXPECT_EQ(m[0].press, 0);
  for (int port = 1; port < 4; ++port)
    EXPECT_EQ(m[port], ALL);
  s.ready = 1;
  m = Gate(Solo(), s);
  EXPECT_EQ(m[0].buttons, START | Z | B);
  // Off the character select, or without the queue's header: nothing.
  View off = Solo();
  off.css = false;
  EXPECT_EQ(Gate(off, s), Masks{});
  View no = Solo();
  no.queue2 = false;
  EXPECT_EQ(Gate(no, s), Masks{});
  EXPECT_EQ(Advance(no, s, 5), State{});
}

TEST(OrcaQueue, BothReadyGoOnAndPort1PressesStart)
{
  State s;
  s = Step(Room(), s, 1000);
  EXPECT_EQ(s.timer_start, 1001u);
  EXPECT_EQ(s.ready, 0);
  s = Step(Press(Room(), START), s, 1010);
  EXPECT_EQ(s.ready, 1);
  EXPECT_EQ(s.timer, 10);
  EXPECT_FALSE(s.flags & FLAG_GO);
  // Port 2's B un-readies only port 2; then its Start: both, and on.
  s = Step(Press(Room(), 0, B), s, 1011);
  EXPECT_EQ(s.ready, 1);
  s = Step(Press(Room(), 0, START), s, 1020);
  EXPECT_EQ(s.ready, 3);
  EXPECT_TRUE(s.flags & FLAG_GO);
  // Start for port 1 every other frame until the screen goes; Start and Z never pass.
  Masks m = Gate(Room(), s);
  EXPECT_EQ(m[0].press & START, s.timer % 2 == 0 ? START : 0);
  EXPECT_TRUE(m[0].buttons & START);
  EXPECT_TRUE(m[1].buttons & (START | Z));
  s = Step(Room(), s, 1021);
  m = Gate(Room(), s);
  EXPECT_EQ(m[0].press & START, s.timer % 2 == 0 ? START : 0);
  // Off the character select: all fresh.
  View off = Room();
  off.css = false;
  EXPECT_EQ(Step(off, s, 1030), State{});
}

TEST(OrcaQueue, CasualTimeoutNamesWhoWasntReadyAndFreezes)
{
  State s = Step(Room(), State{}, 2000);
  s = Step(Press(Room(), START), s, 2001);
  s = Step(Room(), s, 2000 + READY_FRAMES - 1);
  EXPECT_FALSE(s.flags & FLAG_TIMED_OUT);
  s = Step(Room(), s, 2000 + READY_FRAMES);
  EXPECT_TRUE(s.flags & FLAG_TIMED_OUT);
  EXPECT_EQ(s.timeout_who, 2);  // port 2 wasn't ready
  // Frozen: a late Start changes nothing, and nobody's controller does anything.
  const State frozen = Step(Press(Room(), 0, START), s, 2000 + READY_FRAMES + 5);
  EXPECT_EQ(frozen.ready, s.ready);
  EXPECT_FALSE(frozen.flags & FLAG_GO);
  const Masks m = Gate(Room(), frozen);
  EXPECT_EQ(m[0], ALL);
  EXPECT_EQ(m[1], ALL);
  // Neither ready: both named.
  State n = Step(Room(), State{}, 5000);
  n = Step(Room(), n, 5000 + READY_FRAMES);
  EXPECT_EQ(n.timeout_who, 3);
}

TEST(OrcaQueue, RankedTimeoutGoesOnWithThePicks)
{
  State s = Step(Room(true), State{}, 3000);
  s = Step(Room(true), s, 3000 + READY_FRAMES);
  EXPECT_TRUE(s.flags & FLAG_GO);
  EXPECT_FALSE(s.flags & FLAG_TIMED_OUT);
  // Without a character on both, the no-show rule decides: no go.
  View half = Room(true);
  half.ports[1].placed = false;
  State h = Step(half, State{}, 4000);
  h = Step(half, h, 4000 + READY_FRAMES);
  EXPECT_FALSE(h.flags & (FLAG_GO | FLAG_TIMED_OUT));
}

TEST(OrcaQueue, RankedLaterGamesWaitForTheCharacterOrder)
{
  View v = Room(true);
  v.game1 = false;
  v.order_done = false;
  State s = Step(Press(v, START, START), State{}, 6000);
  EXPECT_EQ(s.timer_start, 0u);
  EXPECT_EQ(s.ready, 0);
  EXPECT_FALSE(ReadyPhase(v));
  v.order_done = true;
  s = Step(v, s, 6001);
  EXPECT_EQ(s.timer_start, 6002u);
  s = Step(Press(v, START, START), s, 6002);
  EXPECT_TRUE(s.flags & FLAG_GO);
}

TEST(OrcaQueue, CasualLaterGamesWaitForBothStartsWithNoTimer)
{
  // Game 2 on with the same opponent (Luis, 2026-10-04): no clock, no timeout, nobody frozen.
  View v = Room();
  v.game1 = false;
  EXPECT_TRUE(ReadyPhase(v));
  EXPECT_FALSE(Timed(v));
  State s = Step(v, State{}, 9000);
  s = Step(Press(v, START), s, 9001);
  for (const int frame : {9000 + READY_FRAMES, 9000 + READY_FRAMES * 3, 9000 + READY_FRAMES * 10})
  {
    s = Step(v, s, frame);
    EXPECT_FALSE(s.flags & (FLAG_TIMED_OUT | FLAG_GO)) << frame;
    EXPECT_EQ(s.timeout_who, 0) << frame;
    EXPECT_EQ(s.ready, 1) << frame;
    EXPECT_NE(Gate(v, s)[1], ALL) << frame;
  }
  // Port 2's Start, however late: both ready, on, Start for port 1 every other frame.
  s = Step(Press(v, 0, START), s, 9000 + READY_FRAMES * 10 + 1);
  EXPECT_TRUE(s.flags & FLAG_GO);
  const State next = Step(v, s, 9000 + READY_FRAMES * 10 + 2);
  EXPECT_EQ((Gate(v, s)[0].press | Gate(v, next)[0].press) & START, START);
  // Casual's game 1 and every ranked game keep the 30 s.
  EXPECT_TRUE(Timed(Room()));
  View ranked = Room(true);
  ranked.game1 = false;
  EXPECT_TRUE(Timed(ranked));
  State r = Step(ranked, State{}, 12000);
  r = Step(ranked, r, 12000 + READY_FRAMES);
  EXPECT_TRUE(r.flags & FLAG_GO);
  // From the match block: a fight since the room's header was written is game 2 on.
  FakeMemory m = Block(MB::FLAG_QUEUE2);
  EXPECT_TRUE(Timed(ReadView(m, {})));
  m.Write8(MB::FOUGHT, 1);
  EXPECT_FALSE(ReadView(m, {}).game1);
  EXPECT_FALSE(Timed(ReadView(m, {})));
  m.Write8(MB::MODE, MB::MODE_RANKED);
  EXPECT_TRUE(Timed(ReadView(m, {})));
}

TEST(OrcaQueue, OnlyBothPlayersStartTheTimer)
{
  View v = Room();
  v.plugged = 1;
  State s = Step(Press(v, START), State{}, 7000);
  EXPECT_EQ(s.timer_start, 0u);
  EXPECT_EQ(s.ready, 0);
}

TEST(OrcaQueue, Game1PicksLockOnceIn)
{
  State s = Step(Room(), State{}, 8000);
  EXPECT_EQ(s.locked, 3);
  Masks m = Gate(Room(), s);
  for (int p = 0; p < 2; ++p)
    EXPECT_EQ(m[p].buttons & (A | B | X | Y), A | B | X | Y) << p;
  // Later games: free to change.
  View later = Room();
  later.game1 = false;
  s = Step(later, s, 8001);
  EXPECT_EQ(s.locked, 0);
  m = Gate(later, s);
  EXPECT_EQ(m[0].buttons & (A | X | Y), 0);
}

TEST(OrcaQueue, ThePort2PickIsSteeredIn)
{
  Identity pick;
  pick.character = 0x10;
  pick.costume = 2;
  pick.x = 8.0f;
  pick.y = 6.0f;
  View v = Room();
  v.pick2 = pick;
  // Port 2 just plugged in: its hand below the grid, nothing joined.
  v.ports[1] = CssPort{};
  v.ports[1].readable = true;
  v.ports[1].hand_target = Rules::CSS_HAND_NOTHING;
  v.ports[1].hand_x = -10.0f;
  v.ports[1].hand_y = -20.0f;
  State s = Step(v, State{}, 9000);
  EXPECT_TRUE(s.pick_set);
  EXPECT_EQ(s.locked, 1);
  EXPECT_EQ(s.steer_start, 9001u);
  // The first frames, the stick stays centred (the game takes the new pad's origin).
  Masks m = Gate(v, s);
  EXPECT_EQ(m[1], ALL);
  s = Step(v, s, 9000 + STEER_SETTLE_FRAMES);
  m = Gate(v, s);
  // Its controller does nothing; the stick goes up and right, full tilt.
  EXPECT_EQ(m[1].buttons, Rollback::InputGate::ALL_BUTTONS);
  EXPECT_TRUE(m[1].steer);
  EXPECT_GT(m[1].steer_x, CX + 40);
  EXPECT_GT(m[1].steer_y, CY + 40);
  EXPECT_EQ(m[1].press, 0);
  // It can't ready while being steered.
  s = Step(Press(v, 0, START), s, 9021);
  EXPECT_EQ(s.ready & 2, 0);
  // Over the pick with the token in the hand: still, and A on every fourth frame.
  v.ports[1].hand_target = Rules::CSS_HAND_GRID_HOLDING;
  v.ports[1].human = true;
  v.ports[1].character = 0x10;
  v.ports[1].hand_x = 7.5f;
  v.ports[1].hand_y = 6.2f;
  // Still sliding (it stood elsewhere as the last frame began): no A yet.
  v.hand2_prev_x = 7.1f;
  v.hand2_prev_y = 6.2f;
  for (int f = 9022; f < 9026; ++f)
    EXPECT_EQ(Gate(v, Step(v, s, f))[1].press, 0) << f;
  // Creeping (Brawl's hand with the stick let go): as good as still.
  v.hand2_prev_x = 7.47f;
  v.hand2_prev_y = 6.23f;
  int presses = 0;
  for (int f = 9022; f < 9030; ++f)
  {
    s = Step(v, s, f);
    m = Gate(v, s);
    EXPECT_TRUE(m[1].steer);
    EXPECT_EQ(m[1].steer_x, CX);
    presses += (m[1].press & A) != 0;
  }
  EXPECT_EQ(presses, 2);
  // Down with another costume: X turns it; on the right one: in, and locked.
  v.ports[1].placed = true;
  v.ports[1].costume = 0;
  s = Step(v, s, 9032);
  m = Gate(v, s);
  EXPECT_EQ(s.locked & 2, 0);
  EXPECT_EQ(m[1].press, s.steer % 4 == 0 ? X : 0);
  // Down on another character: B takes it back.
  View wrong = v;
  wrong.ports[1].character = 0x11;
  const State w = Step(wrong, s, 9036);
  EXPECT_EQ(Gate(wrong, w)[1].press, w.steer % 4 == 0 ? B : 0);
  v.ports[1].costume = 2;
  s = Step(v, s, 9040);
  EXPECT_EQ(s.locked, 3);
  m = Gate(v, s);
  EXPECT_FALSE(m[1].steer);
  EXPECT_EQ(m[1].buttons & (A | X | Y), A | X | Y);
  // Now it may ready.
  s = Step(Press(v, 0, START), s, 9041);
  EXPECT_EQ(s.ready & 2, 2);
}

TEST(OrcaQueue, SteeringGivesUpAfterItsLimit)
{
  Identity pick;
  pick.character = 0x10;
  pick.x = 8.0f;
  pick.y = 6.0f;
  View v = Room();
  v.pick2 = pick;
  v.ports[1] = CssPort{};
  State s = Step(v, State{}, 100);
  s = Step(v, s, 100 + STEER_LIMIT_FRAMES + 1);
  EXPECT_FALSE(Gate(v, s)[1].steer);
  // Whatever it put down then is its pick.
  v.ports[1] = Placed(0x20);
  s = Step(v, s, 100 + STEER_LIMIT_FRAMES + 2);
  EXPECT_EQ(s.locked & 2, 2);
}

TEST(OrcaQueue, TheCostumeGivesUpAfterItsLimit)
{
  Identity pick;
  pick.character = 0x10;
  pick.costume = 4;
  pick.x = 8.0f;
  pick.y = 6.0f;
  View v = Room();
  v.pick2 = pick;
  v.ports[1] = Placed(0x10, 0);  // down on the pick, in another colour
  State s = Step(v, State{}, 200);
  EXPECT_EQ(s.costume_start, 201u);
  EXPECT_EQ(s.locked & 2, 0);
  s = Step(v, s, 200 + COSTUME_LIMIT_FRAMES - 1);
  EXPECT_EQ(s.locked & 2, 0);
  s = Step(v, s, 200 + COSTUME_LIMIT_FRAMES);
  EXPECT_EQ(s.locked & 2, 2);
  EXPECT_FALSE(Gate(v, s)[1].steer);
}

TEST(OrcaQueue, PortOnesColourIsAnyColour)
{
  // The same character as port 1 in port 1's colour: the game skips that colour, so any will do.
  Identity pick;
  pick.character = 0x05;
  pick.costume = 2;
  View v = Room();
  v.pick2 = pick;
  v.ports[1] = Placed(0x05, 3);
  const State s = Step(v, State{}, 300);
  EXPECT_EQ(s.locked & 2, 2);
}

TEST(OrcaQueue, SteeringAimsAtThePick)
{
  const auto [cx, cy] = SteerToward(1, 1, 1.05f, 1.05f);
  EXPECT_EQ(cx, CX);
  EXPECT_EQ(cy, CY);
  const auto [lx, ly] = SteerToward(10, 0, 0, 0);
  EXPECT_LT(lx, CX - 90);
  EXPECT_EQ(ly, CY);
  // Near: gentler, but past the dead zone.
  const auto [nx, ny] = SteerToward(0, 0, 0, -1.0f);
  EXPECT_EQ(nx, CX);
  EXPECT_LE(ny, CY - 30);
  EXPECT_GT(ny, CY - 60);
  // A short way off on both axes still moves: the larger component past the dead zone.
  const auto [dx, dy] = SteerToward(-22.22f, 1.49f, -22.5f, 1.78f);
  EXPECT_LE(std::min(dx - CX, dy - CY), -1);
  EXPECT_TRUE(dx <= CX - 29 || dy >= CY + 29) << int(dx) << " " << int(dy);
}

TEST(OrcaQueue, HoldingZSkipsOnce)
{
  SkipHold hold;
  for (int i = 1; i < SKIP_FRAMES; ++i)
    EXPECT_FALSE(hold.Step(true, true)) << i;
  EXPECT_GT(hold.Progress(), 0.9f);
  EXPECT_TRUE(hold.Step(true, true));
  // Held on: once.
  for (int i = 0; i < 200; ++i)
    EXPECT_FALSE(hold.Step(true, true));
  EXPECT_EQ(hold.Progress(), 0.0f);
  // Let go, held again: again.
  EXPECT_FALSE(hold.Step(true, false));
  for (int i = 1; i < SKIP_FRAMES; ++i)
    EXPECT_FALSE(hold.Step(true, true));
  EXPECT_TRUE(hold.Step(true, true));
  // Where no skip may be asked for, holding counts nothing.
  SkipHold other;
  for (int i = 0; i < SKIP_FRAMES * 2; ++i)
    EXPECT_FALSE(other.Step(false, true));
  EXPECT_FALSE(other.Step(true, true));
}

TEST(OrcaQueue, TheLatchKeepsTheRawButtons)
{
  FakeMemory m = Block(MB::FLAG_QUEUE2);
  std::array<std::optional<GCPadStatus>, 4> raw{Pad(START | Z), Pad(B), std::nullopt, Pad(A)};
  EXPECT_GT(Latch(m, raw), 0);
  EXPECT_EQ(m.Read16(RAW), START | Z);
  EXPECT_EQ(m.Read16(RAW + 2), B);
  EXPECT_EQ(m.Read16(RAW_PREV), 0);
  raw = {Pad(0), std::nullopt, std::nullopt, std::nullopt};
  Latch(m, raw);
  EXPECT_EQ(m.Read16(RAW), 0);
  EXPECT_EQ(m.Read16(RAW + 2), 0);
  EXPECT_EQ(m.Read16(RAW_PREV), START | Z);
  EXPECT_EQ(m.Read16(RAW_PREV + 2), B);
  // Without the queue's header (a friends game, an older page's room): never written.
  FakeMemory plain = Block(0);
  EXPECT_EQ(Latch(plain, {Pad(START), Pad(START), std::nullopt, std::nullopt}), 0);
  EXPECT_EQ(plain.Read16(RAW), 0);
}

TEST(OrcaQueue, TheStateRoundTripsThroughTheBlock)
{
  FakeMemory m = Block(MB::FLAG_QUEUE2);
  State s;
  s.ready = 3;
  s.flags = FLAG_GO;
  s.timeout_who = 2;
  s.locked = 1;
  s.timer_start = 0x01020304;
  s.timer = 77;
  s.steer = 12;
  s.steer_start = 0x0A0B0C0D;
  s.costume_start = 0x11223344;
  s.pick_set = true;
  s.pick_character = 0x10;
  s.pick_costume = 3;
  s.pick_x = -4.5f;
  s.pick_y = 9.25f;
  EXPECT_GT(WriteState(m, s), 0);
  EXPECT_EQ(ReadState(m), s);
  EXPECT_EQ(WriteState(m, s), 0);
  // The region fits before the saved rules' flags and after Project+'s saved stage data.
  EXPECT_LE(REGION_END, MB::SAVED_FLAGS);
  EXPECT_GE(MB::QUEUE, MB::SAVED_RSS + MB::SAVED_RSS_SIZE);
}
