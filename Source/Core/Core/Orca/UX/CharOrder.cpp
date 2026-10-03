// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/CharOrder.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/OnlineMenu.h"
#include "Core/Orca/UX/Overlay.h"
#include "InputCommon/GCPadStatus.h"

namespace Orca::UX::CharOrder
{
namespace
{
using Rollback::InputGate::ALL;
using Rollback::InputGate::Mask;
using Rollback::InputGate::Masks;

// The character select (CharOrder.h).
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 MANAGER_SCENE = 0x04;
constexpr u32 SCENE_SELCHAR_TASK = 0x400;
constexpr u32 TASK_AREAS = 0x44;
constexpr u32 AREA_KIND = 0x1B4;
constexpr u32 AREA_CHARACTER = 0x1B8;
constexpr u32 AREA_PLACED = 0x1C0;  // TODO probe
constexpr u32 KIND_HUMAN = 1;
// The game's pads.
constexpr u32 PAD_SYSTEM = 0x805A0040;
constexpr u32 PAD_STATUS = 0x40;   // TODO probe
constexpr u32 PAD_SIZE = 0x40;
constexpr u32 PAD_PRESSED = 0x08;  // TODO probe

// The state's bytes at kArea.
constexpr u32 AT_MAGIC = 0x0;
constexpr u32 AT_GAME = 0x4;
constexpr u32 AT_STEP = 0x5;
constexpr u32 AT_FIRST = 0x6;
constexpr u32 AT_LOCKED = 0x7;
constexpr u32 AT_SINCE = 0x8;
constexpr u32 AT_ELAPSED = 0xC;

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

// The order is on for this set, game and character select (the step machine's and the gate's
// shared condition, so the gate never acts on a state the machine would clear).
bool Applies(const SetView& set, const CssView& css)
{
  return css.on_css && set.ruleset != Ruleset::None && set.game >= 2 && set.game <= 0xFF &&
         (set.last_winner == 0 || set.last_winner == 1) && css.ports[0].human &&
         css.ports[1].human;
}

bool Active(const SetView& set, const CssView& css, const State& state)
{
  return Applies(set, css) && state.step != Step::Idle && state.game == set.game &&
         state.first == set.last_winner;
}

std::string Clock(int frames)
{
  const int seconds = (std::max(frames, 0) + 59) / 60;
  return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}

// ORCA_TEST_CHAR_ORDER's set at its first game, parsed once (a test override).
const SetView& TestSet()
{
  static const SetView set = [] {
    SetView out;
    const std::string spec = TestCharOrderSpec();
    if (!spec.empty() && !ParseTestSpec(spec, &out))
    {
      ERROR_LOG_FMT(ROLLBACK,
                    "Character order: ORCA_TEST_CHAR_ORDER={} is not "
                    "<pplus|brawl>:<game>:<winner port 1-2>; ignored",
                    spec);
      out = {};
    }
    else if (!spec.empty())
    {
      NOTICE_LOG_FMT(ROLLBACK, "Character order: test set ORCA_TEST_CHAR_ORDER={}", spec);
    }
    return out;
  }();
  return set;
}

}  // namespace

State Advance(const SetView& set, const CssView& css, const State& state, int frame)
{
  if (!css.on_css)
    return state;
  if (!Applies(set, css))
    return state == State{} ? state : State{};
  if (state.step == Step::Idle || state.game != set.game || state.first != set.last_winner)
  {
    // A new game's character select: the winner first (Project+), both at once (Brawl). No press
    // counts on this boundary (it was made before the order began), which also keeps a re-run here
    // writing nothing new.
    return State{static_cast<u8>(set.game),
                 set.ruleset == Ruleset::PPlus ? Step::First : Step::Both,
                 static_cast<u8>(set.last_winner), 0, static_cast<u32>(frame), 0};
  }
  State s = state;
  const u32 now = static_cast<u32>(frame);
  const u32 elapsed = now >= s.since ? now - s.since : 0;
  s.elapsed = static_cast<u16>(std::min<u32>(elapsed, 0xFFFF));
  if (elapsed == 0 || s.step == Step::Done)
    return s;
  // A lock-in: A newly pressed on a frame of this step with the token on a character (A on a
  // character picks it, A anywhere else keeps the one the token is on).
  const auto locks_in = [&css](int port) {
    const CssPort& p = css.ports[port];
    return (p.pressed & PAD_BUTTON_A) != 0 && p.placed;
  };
  const bool timed_out = elapsed >= static_cast<u32>(PICK_FRAMES);
  bool next = timed_out;
  if (s.step == Step::Both)
  {
    for (int port = 0; port < 2; ++port)
    {
      if (locks_in(port))
        s.locked = static_cast<u8>(s.locked | (1 << port));
    }
    next = next || s.locked == 3;
  }
  else
  {
    next = next || locks_in(s.step == Step::First ? s.first : 1 - s.first);
  }
  if (next)
  {
    s.step = s.step == Step::First ? Step::Second : Step::Done;
    s.since = now;
    s.elapsed = 0;
  }
  return s;
}

Masks Gate(const SetView& set, const CssView& css, const State& state)
{
  Masks masks{};
  if (!Active(set, css, state))
    return masks;
  const int winner = state.first;
  const int loser = 1 - winner;
  switch (state.step)
  {
  case Step::First:
    masks[loser] = ALL;
    masks[winner].buttons = PAD_BUTTON_B | PAD_BUTTON_START;
    break;
  case Step::Second:
    masks[winner] = ALL;
    masks[loser].buttons = PAD_BUTTON_B;
    break;
  case Step::Both:
    // Neither takes the token back or goes on; a player who locked in can't change.
    for (int port = 0; port < 2; ++port)
    {
      if (state.locked & (1 << port))
        masks[port] = ALL;
      else
        masks[port].buttons = PAD_BUTTON_B | PAD_BUTTON_START;
    }
    break;
  case Step::Done:
    masks[winner] = ALL;
    masks[loser] = ALL;
    // Start for the loser on every other frame: each is a new press, until the screen goes.
    if (state.elapsed % 2 == 0)
      masks[loser].press = PAD_BUTTON_START;
    break;
  case Step::Idle:
    break;
  }
  return masks;
}

std::string Line(const SetView& set, const CssView& css, const State& state,
                 const std::vector<Events::PortInfo>& ports)
{
  if (!Active(set, css, state) || state.step == Step::Done)
    return {};
  // Each port's name ("P<n>" without one), and this machine's port (-1: neither; a harness run).
  std::array<std::string, 2> names{"P1", "P2"};
  int local = -1;
  for (const Events::PortInfo& p : ports)
  {
    if (p.port < 0 || p.port > 1)
      continue;
    if (!p.name.empty())
      names[p.port] = p.name;
    if (!p.remote)
      local = p.port;
  }
  const std::string left = Clock(PICK_FRAMES - state.elapsed);
  if (state.step == Step::Both)
  {
    if (local < 0)
      return fmt::format("Characters · {}", left);
    if (state.locked & (1 << local))
      return fmt::format("Waiting for {} · {}", names[1 - local], left);
    return fmt::format("Pick your character · {} · A locks it in", left);
  }
  const int port = state.step == Step::First ? state.first : 1 - state.first;
  if (port == local)
  {
    return fmt::format("{} · {} · A locks it in",
                       state.step == Step::First ? "You pick first" : "Your pick", left);
  }
  return fmt::format("{} {} · {}", names[port],
                     state.step == Step::First ? "picks first" : "picks", left);
}

int MkFreePicker(const std::array<int, 2>& characters)
{
  const bool a = characters[0] == kMetaKnight;
  const bool b = characters[1] == kMetaKnight;
  if (a == b)
    return -1;
  return a ? 1 : 0;
}

SetView ReadSet(const GuestMemory& memory)
{
  // Harness runs: the test knob's set.
  if (TestSet().ruleset != Ruleset::None)
    return TestSet();
  // The ranked set state lives in the match block (the ranked rules' Phase 2), which this branch
  // doesn't have yet: until it does, no set, and the order never runs.
  (void)memory;
  return {};
}

CssView ReadCss(const GuestMemory& m)
{
  CssView css;
  if (ReadSceneName(m) != "scSelctCharacter")
    return css;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + MANAGER_SCENE))
    return css;
  const u32 scene = m.Read32(manager + MANAGER_SCENE);
  if (!Pointer(m, scene + SCENE_SELCHAR_TASK))
    return css;
  const u32 task = m.Read32(scene + SCENE_SELCHAR_TASK);
  if (!Pointer(m, task + TASK_AREAS + 4))
    return css;
  css.on_css = true;
  for (u32 i = 0; i < 2; ++i)
  {
    const u32 area = m.Read32(task + TASK_AREAS + 4 * i);
    CssPort& p = css.ports[i];
    if (Pointer(m, area + AREA_KIND) && Pointer(m, area + AREA_CHARACTER) &&
        Pointer(m, area + AREA_PLACED))
    {
      p.human = m.Read32(area + AREA_KIND) == KIND_HUMAN;
      p.character = static_cast<int>(m.Read32(area + AREA_CHARACTER));
      p.placed = m.Read32(area + AREA_PLACED) != 0;
    }
  }
  if (Pointer(m, PAD_SYSTEM))
  {
    const u32 pads = m.Read32(PAD_SYSTEM);
    for (u32 i = 0; i < 2; ++i)
    {
      const u32 at = pads + PAD_STATUS + i * PAD_SIZE + PAD_PRESSED;
      if (Pointer(m, at))
        css.ports[i].pressed = static_cast<u16>(m.Read32(at));
    }
  }
  return css;
}

State ReadState(const GuestMemory& m)
{
  State s;
  if (!m.Valid(kArea) || !m.Valid(kArea + 0xF) || m.Read32(kArea + AT_MAGIC) != kMagic)
    return s;
  const u8 step = m.Read8(kArea + AT_STEP);
  if (step > static_cast<u8>(Step::Both))
    return s;
  s.game = m.Read8(kArea + AT_GAME);
  s.step = static_cast<Step>(step);
  s.first = m.Read8(kArea + AT_FIRST) & 1;
  s.locked = m.Read8(kArea + AT_LOCKED) & 3;
  s.since = m.Read32(kArea + AT_SINCE);
  s.elapsed = m.Read16(kArea + AT_ELAPSED);
  return s;
}

int WriteState(GuestMemory& m, const State& s)
{
  if (!m.Valid(kArea) || !m.Valid(kArea + 0xF))
    return 0;
  int changed = 0;
  const auto put8 = [&](u32 at, u8 v) {
    if (m.Read8(kArea + at) != v)
    {
      m.Write8(kArea + at, v);
      ++changed;
    }
  };
  const auto put32 = [&](u32 at, u32 v) {
    for (u32 i = 0; i < 4; ++i)
      put8(at + i, static_cast<u8>(v >> (24 - 8 * i)));
  };
  put32(AT_MAGIC, kMagic);
  put8(AT_GAME, s.game);
  put8(AT_STEP, static_cast<u8>(s.step));
  put8(AT_FIRST, s.first);
  put8(AT_LOCKED, s.locked);
  put32(AT_SINCE, s.since);
  put8(AT_ELAPSED, static_cast<u8>(s.elapsed >> 8));
  put8(AT_ELAPSED + 1, static_cast<u8>(s.elapsed));
  put8(AT_ELAPSED + 2, 0);
  put8(AT_ELAPSED + 3, 0);
  return changed;
}

void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports)
{
  GuardMemory m(guard);
  const SetView set = ReadSet(m);
  const CssView css = ReadCss(m);
  const State before = ReadState(m);
  const State after = Advance(set, css, before, frame);
  if (after != before)
    WriteState(m, after);
  if (!resimulating)
    SetRuleLine(Line(set, css, after, ports));
}

Masks GateFrame(const Core::CPUThreadGuard& guard)
{
  GuardMemory m(guard);
  const SetView set = ReadSet(m);
  if (set.ruleset == Ruleset::None)
    return {};
  return Gate(set, ReadCss(m), ReadState(m));
}

bool ParseTestSpec(const std::string& spec, SetView* out)
{
  const size_t a = spec.find(':');
  const size_t b = a == std::string::npos ? a : spec.find(':', a + 1);
  if (b == std::string::npos || spec.find(':', b + 1) != std::string::npos)
    return false;
  const std::string_view rules = std::string_view(spec).substr(0, a);
  const std::string game = spec.substr(a + 1, b - a - 1);
  const std::string winner = spec.substr(b + 1);
  SetView set;
  if (rules == "pplus")
    set.ruleset = Ruleset::PPlus;
  else if (rules == "brawl")
    set.ruleset = Ruleset::Brawl;
  else
    return false;
  if (game.empty() || game.size() > 2 ||
      !std::all_of(game.begin(), game.end(), [](char c) { return c >= '0' && c <= '9'; }))
    return false;
  set.game = std::atoi(game.c_str());
  if (set.game < 1 || winner.size() != 1 || winner[0] < '1' || winner[0] > '2')
    return false;
  set.last_winner = winner[0] - '1';
  *out = set;
  return true;
}
}  // namespace Orca::UX::CharOrder
