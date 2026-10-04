// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Queue.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <map>
#include <mutex>
#include <utility>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/CharOrder.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/OnlineMenu.h"
#include "Core/Orca/UX/OnlineRules.h"
#include "Core/Orca/UX/Overlay.h"

namespace Orca::UX::Queue
{
namespace
{
using Rollback::InputGate::ALL;
using Rollback::InputGate::Mask;
using Rollback::InputGate::Masks;

// The character select (CharOrder.h, OnlineRules.h; BrawlHeaders' muSelCharPlayerArea).
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 MANAGER_SCENE = 0x04;
constexpr u32 SCENE_SELCHAR_TASK = 0x400;
constexpr u32 TASK_AREAS = 0x44;
constexpr u32 AREA_KIND = 0x1B4;
constexpr u32 AREA_CHARACTER = 0x1B8;
constexpr u32 AREA_COSTUME = 0x1BC;  // m_charColorNo
constexpr u32 AREA_IN_HAND = 0x1F8;  // u8: 1 the token is in the hand, 0 down
constexpr u32 AREA_FLYING = 0x1F9;   // u8: 1 while the token flies
constexpr u32 KIND_HUMAN = 1;

constexpr u16 BUTTON_START = PAD_BUTTON_START;
constexpr u16 BUTTON_B = PAD_BUTTON_B;
constexpr u16 BUTTON_Z = PAD_TRIGGER_Z;

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

void Put32(std::vector<u8>& out, u32 v)
{
  out.push_back(static_cast<u8>(v >> 24));
  out.push_back(static_cast<u8>(v >> 16));
  out.push_back(static_cast<u8>(v >> 8));
  out.push_back(static_cast<u8>(v));
}

u32 Get32(const std::vector<u8>& b, size_t at)
{
  return u32(b[at]) << 24 | u32(b[at + 1]) << 16 | u32(b[at + 2]) << 8 | u32(b[at + 3]);
}

// Writes only what differs, counting the bytes changed.
struct Writer
{
  GuestMemory& m;
  int changed = 0;
  void U8(u32 a, u8 v)
  {
    if (!m.Valid(a) || m.Read8(a) == v)
      return;
    m.Write8(a, v);
    ++changed;
  }
  void U16(u32 a, u16 v)
  {
    U8(a, static_cast<u8>(v >> 8));
    U8(a + 1, static_cast<u8>(v));
  }
  void U32(u32 a, u32 v)
  {
    U16(a, static_cast<u16>(v >> 16));
    U16(a + 2, static_cast<u16>(v));
  }
};

bool RegionMapped(const GuestMemory& m)
{
  return m.Valid(MatchBlock::QUEUE) && m.Valid(REGION_END - 1);
}

std::string Clock(int frames)
{
  const int seconds = (std::max(frames, 0) + 59) / 60;
  return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}
}  // namespace

// ---- The queue identity ----

std::vector<u8> EncodeIdentity(const Identity& id)
{
  std::vector<u8> out;
  out.push_back(1);
  const u16 rating = id.rating < 0 || id.rating > 0xFFFE ? 0xFFFF : static_cast<u16>(id.rating);
  out.push_back(static_cast<u8>(rating >> 8));
  out.push_back(static_cast<u8>(rating));
  out.push_back(id.HasPick() && id.character <= 0xFE ? static_cast<u8>(id.character) : 0xFF);
  out.push_back(static_cast<u8>(std::clamp(id.costume, 0, 0xFF)));
  Put32(out, std::bit_cast<u32>(id.x));
  Put32(out, std::bit_cast<u32>(id.y));
  return out;
}

std::optional<Identity> DecodeIdentity(const std::vector<u8>& b)
{
  // A newer version may add bytes after these: what this one knows is read.
  if (b.size() < 13 || b[0] != 1)
    return std::nullopt;
  Identity id;
  const u16 rating = static_cast<u16>(b[1] << 8 | b[2]);
  id.rating = rating == 0xFFFF ? -1 : rating;
  id.character = b[3] == 0xFF ? -1 : b[3];
  id.costume = b[4];
  id.x = std::bit_cast<float>(Get32(b, 5));
  id.y = std::bit_cast<float>(Get32(b, 9));
  // Anything a game can't hold is no place: the player then picks by hand.
  if (!std::isfinite(id.x) || !std::isfinite(id.y) || std::fabs(id.x) > 100 ||
      std::fabs(id.y) > 100)
  {
    id.character = -1;
    id.x = id.y = 0;
  }
  return id;
}

// ---- The step machine ----

bool ReadyPhase(const View& v)
{
  return v.queue2 && !v.solo && v.css && (v.plugged & 3) == 3 && (v.game1 || v.order_done);
}

bool Timed(const View& v)
{
  return v.ranked || v.game1;
}

State Advance(const View& v, const State& state, int frame)
{
  State s = state;
  if (!v.queue2)
    return State{};
  // Off the character select: a fresh select next time (a game, the stage select, the menus).
  if (!v.css)
  {
    s = State{};
    return s;
  }
  std::array<u16, 2> newly{};
  for (int p = 0; p < 2; ++p)
    newly[p] = static_cast<u16>(v.raw[p] & ~v.raw_prev[p]);
  const u32 now = static_cast<u32>(frame) + 1;
  // A port's ready flag: Start with its token down readies it, B or the token taken up again
  // un-readies it (B wins a frame that has both).
  const auto follow = [&](int p, bool may_ready) {
    bool ready = (s.ready >> p) & 1;
    const bool placed = v.ports[p].placed;
    if (ready && ((newly[p] & BUTTON_B) || !placed || !may_ready))
      ready = false;
    else if (!ready && may_ready && placed && (newly[p] & BUTTON_START) && !(newly[p] & BUTTON_B))
      ready = true;
    s.ready = static_cast<u8>((s.ready & ~(1 << p)) | (ready ? 1 << p : 0));
  };

  if (v.solo)
  {
    // The queue's own character select: port 1 alone. The B that stops the search is swallowed
    // until it is let go.
    const u8 ready = s.ready;
    const bool swallowing = (s.flags & FLAG_SWALLOW_B) && (v.raw[0] & BUTTON_B);
    s = State{};
    s.ready = ready & 1;
    follow(0, true);
    if (swallowing || ((ready & 1) && !(s.ready & 1) && (newly[0] & BUTTON_B)))
      s.flags |= FLAG_SWALLOW_B;
    return s;
  }

  // ---- The room's character select ----
  const bool both = (v.plugged & 3) == 3;
  // Port 2's pick from its queue identity (synced: the same at this frame everywhere), game 1 only.
  s.pick_set = v.game1 && v.pick2 && v.pick2->HasPick();
  if (s.pick_set)
  {
    s.pick_character = static_cast<u8>(v.pick2->character);
    s.pick_costume = static_cast<u8>(v.pick2->costume);
    s.pick_x = v.pick2->x;
    s.pick_y = v.pick2->y;
  }
  else
  {
    s.pick_character = s.pick_costume = 0;
    s.pick_x = s.pick_y = 0;
  }
  // Port 2's steering clock: from the first boundary its pick is to be put in (game 1).
  if (v.game1 && both && s.pick_set && !(s.locked & 2) && s.steer_start == 0)
    s.steer_start = now;
  if (s.steer_start != 0)
  {
    const u32 t = now >= s.steer_start ? now - s.steer_start : 0;
    s.steer = static_cast<u16>(std::min<u32>(t, 0xFFFF));
  }
  if (v.game1 && both)
  {
    // Game 1's picks lock once they are in: the host's as it came, the joiner's once the steering
    // put it down (or gave up, or there was nothing to steer to).
    if (v.ports[0].placed)
      s.locked |= 1;
    const CssPort& two = v.ports[1];
    const bool steering_over = s.steer_start != 0 && s.steer >= STEER_LIMIT_FRAMES;
    const bool on_pick = s.pick_set && two.placed && two.character == s.pick_character;
    // The same character as port 1 can't wear port 1's colour: the game skips it, so any colour
    // will do then.
    const bool colour_taken = v.ports[0].placed && v.ports[0].character == s.pick_character &&
                              v.ports[0].costume == s.pick_costume;
    // The costume's own clock, from the first boundary the token is down on the pick.
    if (on_pick && !(s.locked & 2) && s.costume_start == 0)
      s.costume_start = now;
    const bool costume_over =
        s.costume_start != 0 && now - std::min(now, s.costume_start) >= COSTUME_LIMIT_FRAMES;
    if (two.placed && (!s.pick_set || steering_over ||
                       (on_pick &&
                        (two.costume == s.pick_costume || colour_taken || costume_over))))
    {
      s.locked |= 2;
    }
  }
  else if (!v.game1)
  {
    s.locked = 0;
  }

  if (!ReadyPhase(v))
  {
    // Not both players in (yet, or any more), or ranked's character order still running: nobody
    // is ready, no timer. A casual timeout stays (the pages are leaving the room).
    s.ready = 0;
    s.flags &= FLAG_TIMED_OUT;
    if (!(s.flags & FLAG_TIMED_OUT))
      s.timeout_who = 0;
    s.timer_start = 0;
    s.timer = 0;
    return s;
  }
  // A start from another frame numbering (later than now, or far longer ago than the timer runs)
  // starts here. Untimed (casual's later games) it still counts: Start's every other frame below.
  if (s.timer_start == 0 || s.timer_start > now ||
      now - s.timer_start > static_cast<u32>(READY_FRAMES) * 4)
  {
    s.timer_start = now;
  }
  const u32 t = now - s.timer_start;
  s.timer = static_cast<u16>(std::min<u32>(t, 0xFFFF));
  if (s.flags & (FLAG_TIMED_OUT | FLAG_GO))
    return s;
  // A joiner whose pick is still being put in can't ready yet.
  const bool steering = v.game1 && s.pick_set && !(s.locked & 2);
  follow(0, true);
  follow(1, !steering);
  if ((s.ready & 3) == 3)
  {
    s.flags |= FLAG_GO;
  }
  else if (Timed(v) && t >= static_cast<u32>(READY_FRAMES))
  {
    if (v.ranked)
    {
      // Ranked goes on with the picks as they are; without a character on both, the no-show rule
      // (OnlineRules.h) decides.
      if (v.ports[0].placed && v.ports[1].placed)
        s.flags |= FLAG_GO;
    }
    else
    {
      s.flags |= FLAG_TIMED_OUT;
      s.timeout_who = static_cast<u8>(~s.ready & 3);
    }
  }
  return s;
}

bool SkipHold::Step(bool may_skip, bool z_held)
{
  if (!may_skip || !z_held)
  {
    frames = 0;
    sent = false;
    return false;
  }
  ++frames;
  if (frames >= SKIP_FRAMES && !sent)
  {
    sent = true;
    return true;
  }
  return false;
}

float SkipHold::Progress() const
{
  if (sent || frames <= 0)
    return 0;
  return std::min(1.0f, static_cast<float>(frames) / SKIP_FRAMES);
}

std::pair<u8, u8> SteerToward(float x, float y, float tx, float ty)
{
  const float dx = tx - x, dy = ty - y;
  const float d = std::sqrt(dx * dx + dy * dy);
  if (!std::isfinite(d) || d < 0.1f)
    return {GCPadStatus::MAIN_STICK_CENTER_X, GCPadStatus::MAIN_STICK_CENTER_Y};
  // Full tilt (the hand's 0.83 or 1.0 a frame) from 3 units out, slower nearer. The larger
  // component is at least 30, past the game's dead zone (30 moves Brawl's hand about 0.06 a frame,
  // fine enough to stop on a spot), so the hand always closes in; a smaller one the dead zone
  // swallows is taken up once it is the larger.
  const float mag = std::clamp(100.0f * d / 3.0f, 30.0f, 100.0f);
  float cx = dx / d * mag, cy = dy / d * mag;
  const float larger = std::max(std::fabs(cx), std::fabs(cy));
  if (larger < 30.0f)
  {
    cx *= 30.0f / larger;
    cy *= 30.0f / larger;
  }
  const auto axis = [](float c, u8 centre) {
    return static_cast<u8>(std::clamp(static_cast<int>(std::lround(centre + c)), 1, 255));
  };
  return {axis(cx, GCPadStatus::MAIN_STICK_CENTER_X), axis(cy, GCPadStatus::MAIN_STICK_CENTER_Y)};
}

Masks Gate(const View& v, const State& s)
{
  Masks masks{};
  if (!v.queue2 || !v.css)
    return masks;
  if (v.solo)
  {
    // Port 1 alone: Start readies (Orca's, never the game's), Z is the room's skip, and while
    // ready B only stops the search.
    masks[0].buttons =
        BUTTON_START | BUTTON_Z | ((s.ready & 1) || (s.flags & FLAG_SWALLOW_B) ? BUTTON_B : 0);
    for (int port = 1; port < Rollback::InputGate::PORTS; ++port)
      masks[port] = ALL;
    return masks;
  }
  masks[0].buttons = BUTTON_START | BUTTON_Z;
  masks[1].buttons = BUTTON_START | BUTTON_Z;
  masks[2] = ALL;
  masks[3] = ALL;
  if (s.flags & FLAG_TIMED_OUT)
  {
    // Nothing moves until the pages leave the room.
    masks[0] = ALL;
    masks[1] = ALL;
    return masks;
  }
  if (v.game1)
  {
    // Game 1's characters, once in: no token pickup, no costume.
    for (int p = 0; p < 2; ++p)
    {
      if (s.locked & (1 << p))
        masks[p].buttons |= PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y;
    }
    // Port 2's pick, put in for it: its own controller does nothing meanwhile.
    if (s.pick_set && !(s.locked & 2) && s.steer < STEER_LIMIT_FRAMES && (v.plugged & 2))
    {
      Mask& two = masks[1];
      two = ALL;
      const CssPort& p = v.ports[1];
      const bool edge = (s.steer & 3) == 0;  // a press every fourth frame, released between
      two.steer = s.steer >= STEER_SETTLE_FRAMES;
      if (!two.steer)
      {
        // Centred while the game takes the new pad's origin (ALL centres the stick).
      }
      else if (p.placed)
      {
        // Down on another character: back into the hand. On it with another costume: X turns it.
        if (p.character != s.pick_character)
          two.press = edge ? BUTTON_B : 0;
        else if (p.costume != s.pick_costume)
          two.press = edge ? PAD_BUTTON_X : 0;
      }
      else if (p.hand_target == Rules::CSS_HAND_GRID_HOLDING && p.character == s.pick_character)
      {
        // Over the pick with the token in the hand: still, and A puts it down once the hand has
        // all but stopped (since the last frame began: the character it reads lags the hand by a
        // frame, so a hand still moving can put the token on the next one). With the stick let go
        // Brawl's hand still creeps, about 0.03 a frame, which is fine.
        const bool still = std::fabs(p.hand_x - v.hand2_prev_x) < 0.05f &&
                           std::fabs(p.hand_y - v.hand2_prev_y) < 0.05f;
        two.press = edge && still ? PAD_BUTTON_A : 0;
      }
      else
      {
        const auto [x, y] = SteerToward(p.hand_x, p.hand_y, s.pick_x, s.pick_y);
        two.steer_x = x;
        two.steer_y = y;
      }
    }
  }
  if (s.flags & FLAG_GO)
  {
    // Both ready: Start for port 1, every other frame until the character select goes.
    if ((s.timer & 1) == 0)
      masks[0].press |= BUTTON_START;
  }
  return masks;
}

// ---- Memory ----

View ReadView(const GuestMemory& m, const std::vector<Events::PortInfo>& ports)
{
  View v;
  const Rules::Header h = Rules::ReadHeader(m);
  if (!h.Queue2() || !RegionMapped(m))
    return v;
  v.queue2 = true;
  v.solo = h.Solo();
  v.ranked = h.mode == Rules::Mode::Ranked;
  v.game1 = (m.Read8(MatchBlock::FOUGHT) & 1) == 0;
  v.plugged = m.Read8(MatchBlock::PLUGGED);
  for (int p = 0; p < 2; ++p)
  {
    v.raw[p] = m.Read16(RAW + 2 * static_cast<u32>(p));
    v.raw_prev[p] = m.Read16(RAW_PREV + 2 * static_cast<u32>(p));
  }
  v.hand2_prev_x = std::bit_cast<float>(m.Read32(HAND_PREV));
  v.hand2_prev_y = std::bit_cast<float>(m.Read32(HAND_PREV + 4));
  if (v.ranked && !v.game1)
  {
    const CharOrder::Step step = CharOrder::ReadState(m).step;
    v.order_done = step != CharOrder::Step::First && step != CharOrder::Step::Second;
  }
  for (const Events::PortInfo& info : ports)
  {
    if (info.port != 1 || info.queue.empty())
      continue;
    if (const auto id = DecodeIdentity(info.queue); id && id->HasPick())
      v.pick2 = id;
  }
  if (ReadSceneName(m) != "scSelctCharacter" || !Pointer(m, SCENE_MANAGER))
    return v;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + MANAGER_SCENE))
    return v;
  const u32 scene = m.Read32(manager + MANAGER_SCENE);
  if (!Pointer(m, scene + SCENE_SELCHAR_TASK))
    return v;
  const u32 task = m.Read32(scene + SCENE_SELCHAR_TASK);
  if (!Pointer(m, task + TASK_AREAS + 4))
    return v;
  v.css = true;
  for (u32 i = 0; i < 2; ++i)
  {
    const u32 area = m.Read32(task + TASK_AREAS + 4 * i);
    CssPort& p = v.ports[i];
    if (!Pointer(m, area + AREA_KIND) || !Pointer(m, area + AREA_CHARACTER) ||
        !Pointer(m, area + AREA_COSTUME) || !m.Valid(area + AREA_FLYING))
    {
      continue;
    }
    p.readable = true;
    p.human = m.Read32(area + AREA_KIND) == KIND_HUMAN;
    p.character = static_cast<int>(m.Read32(area + AREA_CHARACTER));
    p.costume = static_cast<int>(m.Read32(area + AREA_COSTUME));
    p.placed = p.human && p.character != NO_CHARACTER && m.Read8(area + AREA_IN_HAND) == 0 &&
               m.Read8(area + AREA_FLYING) == 0;
    const Rules::CssHand hand = Rules::ReadCssHand(m, static_cast<int>(i));
    if (hand.valid)
    {
      p.hand_target = hand.target;
      p.hand_x = hand.x;
      p.hand_y = hand.y;
    }
  }
  return v;
}

State ReadState(const GuestMemory& m)
{
  State s;
  if (!RegionMapped(m))
    return s;
  s.ready = m.Read8(READY);
  s.flags = m.Read8(FLAGS);
  s.timeout_who = m.Read8(TIMEOUT_WHO);
  s.locked = m.Read8(LOCKED);
  s.timer_start = m.Read32(TIMER_START);
  s.timer = m.Read16(TIMER);
  s.steer = m.Read16(STEER);
  s.steer_start = m.Read32(STEER_START);
  s.costume_start = m.Read32(COSTUME_START);
  s.pick_set = m.Read8(PICK) == 1;
  s.pick_character = m.Read8(PICK + 1);
  s.pick_costume = m.Read8(PICK + 2);
  s.pick_x = std::bit_cast<float>(m.Read32(PICK + 4));
  s.pick_y = std::bit_cast<float>(m.Read32(PICK + 8));
  return s;
}

int WriteState(GuestMemory& m, const State& s)
{
  if (!RegionMapped(m))
    return 0;
  Writer w{m};
  w.U8(READY, s.ready);
  w.U8(FLAGS, s.flags);
  w.U8(TIMEOUT_WHO, s.timeout_who);
  w.U8(LOCKED, s.locked);
  w.U32(TIMER_START, s.timer_start);
  w.U16(TIMER, s.timer);
  w.U16(STEER, s.steer);
  w.U32(STEER_START, s.steer_start);
  w.U32(COSTUME_START, s.costume_start);
  w.U8(PICK, s.pick_set ? 1 : 0);
  w.U8(PICK + 1, s.pick_character);
  w.U8(PICK + 2, s.pick_costume);
  w.U8(PICK + 3, 0);
  w.U32(PICK + 4, std::bit_cast<u32>(s.pick_x));
  w.U32(PICK + 8, std::bit_cast<u32>(s.pick_y));
  return w.changed;
}

int Latch(GuestMemory& m, const std::array<std::optional<GCPadStatus>, 4>& raw)
{
  if (!Rules::ReadHeader(m).Queue2() || !RegionMapped(m))
    return 0;
  Writer w{m};
  for (u32 p = 0; p < 2; ++p)
  {
    const u16 old = m.Read16(RAW + 2 * p);
    const u16 now =
        raw[p] && raw[p]->isConnected ? static_cast<u16>(raw[p]->button & Rollback::InputGate::ALL_BUTTONS) :
                                        0;
    w.U16(RAW_PREV + 2 * p, old);
    w.U16(RAW + 2 * p, now);
  }
  // Port 2's hand as this frame starts (the steering's A waits for it to stand still).
  const Rules::CssHand hand = Rules::ReadCssHand(m, 1);
  w.U32(HAND_PREV, std::bit_cast<u32>(hand.valid ? hand.x : 0.0f));
  w.U32(HAND_PREV + 4, std::bit_cast<u32>(hand.valid ? hand.y : 0.0f));
  return w.changed;
}

// ---- The app's side and the local lines ----

namespace
{
std::mutex s_mutex;
bool s_active = false;
bool s_ranked = false;
int s_rating = -1;
// The pick this player made on the queue's own character select (and where its hand put it).
Identity s_pick;
std::atomic<bool> s_solo_ready{false};
std::atomic<bool> s_on_solo_css{false};
std::atomic<bool> s_clear_ready{false};
std::atomic<bool> s_last_timeout_mine{false};

// First-run, local: what was printed, Z held for a skip, where the hand held the token.
struct Local
{
  bool printed_ready = false;
  Identity printed;
  SkipHold skip;
  std::array<bool, 2> was_placed{};
  // Each port's hand while it holds its token over one character: that character, where the hand
  // first read it and where it last did. The pick's place is the middle of the two, inside the
  // character's square (the hand's own place can sit on its edge, and the character read lags the
  // hand by a frame).
  struct Hover
  {
    int character = -1;
    std::pair<float, float> first{}, last{};
    std::pair<float, float> Middle() const
    {
      return {(first.first + last.first) / 2, (first.second + last.second) / 2};
    }
  };
  std::array<Hover, 2> hover{};
  // The place the solo character select's pick went down at (its hover's middle).
  std::pair<float, float> pick_place{};
  int pick_place_character = -1;
};
Local& L()
{
  static Local l;
  return l;
}

// The casual timeout per frame, until it is final (as OnlineRules' no-show).
struct TimeoutTracker
{
  std::map<int, u8> pending;  // bit 7: timed out; bits 0-1: who wasn't ready
  u64 resyncs = 0;
  bool started = false;
  u8 last = 0;
  int last_frame = -1;
};
TimeoutTracker& Timeouts()
{
  static TimeoutTracker t;
  return t;
}

void PublishIdentity()
{
  Identity id;
  {
    std::lock_guard lk(s_mutex);
    id = s_pick;
    id.rating = s_rating;
  }
  Orca::Events::SetOwnQueue(id.rating < 0 && !id.HasPick() ? std::vector<u8>{} :
                                                              EncodeIdentity(id));
}

std::string NameFor(const std::vector<Events::PortInfo>& ports, int port, int* rating)
{
  for (const Events::PortInfo& p : ports)
  {
    if (p.port != port)
      continue;
    if (rating)
    {
      if (const auto id = DecodeIdentity(p.queue))
        *rating = id->rating;
    }
    if (!p.name.empty())
      return p.name;
  }
  return fmt::format("Player {}", port + 1);
}

// The queue's own character select, where the player's pick (and its place) is taken.
bool solo_css_for_pick(const View& v)
{
  return v.queue2 && v.solo && v.css;
}

void Lines(const View& v, const State& s, const std::vector<Events::PortInfo>& ports, int local)
{
  std::string line, note;
  std::array<std::string, 4> labels;
  const std::string mode = v.ranked ? "Ranked" : "Casual";
  if (v.queue2 && v.css && v.solo)
  {
    if (s.ready & 1)
    {
      line = fmt::format("{} · Searching for an opponent…", mode);
      note = "B to cancel";
    }
    else
    {
      line = mode;
      note = "Pick your character, then press Start to search";
    }
  }
  else if (v.queue2 && v.css)
  {
    // Each player's name near their panel; in ranked, their rating too ("ada · 1532").
    for (int p = 0; p < 2; ++p)
    {
      if (!(v.plugged & (1 << p)))
        continue;
      int rating = -1;
      const std::string name = NameFor(ports, p, &rating);
      labels[p] = v.ranked && rating >= 0 ? fmt::format("{} · {}", name, rating) : name;
      if (s.ready & (1 << p))
        labels[p] += " ✓";
    }
    const int other = local == 0 ? 1 : 0;
    const std::string them = NameFor(ports, other, nullptr);
    if ((v.plugged & 3) != 3)
    {
      line = fmt::format("{} · Opponent found", mode);
      note = fmt::format("Waiting for {}…", them);
    }
    else if (s.flags & FLAG_TIMED_OUT)
    {
      line = "Time's up";
      note = (s.timeout_who >> local) & 1 ? "You didn't ready up" :
                                            fmt::format("{} didn't ready up", them);
    }
    else if (s.flags & FLAG_GO)
    {
      line = fmt::format("{} · Here we go", mode);
    }
    else if (ReadyPhase(v))
    {
      const bool mine = (s.ready >> local) & 1;
      const bool theirs = (s.ready >> other) & 1;
      if (Timed(v))
      {
        const std::string left = Clock(READY_FRAMES - s.timer);
        line = mine ? fmt::format("Ready · {}", left) :
                      fmt::format("Press Start when you're ready · {}", left);
        note = theirs ? fmt::format("{} is ready", them) : fmt::format("Waiting for {}", them);
      }
      else
      {
        // Casual's later games: no clock; both press Start when they're done talking.
        line = mine ? "Ready" : "Press Start when you're ready";
        note = theirs ? fmt::format("{} is ready", them) :
                        fmt::format("Waiting for {} to press Start", them);
      }
      const Local& l = L();
      if (l.skip.Progress() > 0)
      {
        const int bars = std::clamp(static_cast<int>(l.skip.Progress() * 10), 0, 10);
        note = "Finding someone else " + std::string(static_cast<size_t>(bars), '|') +
               std::string(static_cast<size_t>(10 - bars), '.');
      }
      else
      {
        note += " · Hold Z to find someone else";
      }
    }
  }
  SetQueueLines(std::move(line), std::move(note), std::move(labels));
}
}  // namespace

void Begin(bool ranked)
{
  {
    std::lock_guard lk(s_mutex);
    s_active = true;
    s_ranked = ranked;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Queue: on the {} queue's own character select", ranked ? "ranked" :
                                                                                       "casual");
}

void End()
{
  {
    std::lock_guard lk(s_mutex);
    s_active = false;
    s_pick = {};
  }
  s_solo_ready = false;
  PublishIdentity();
}

bool Active()
{
  std::lock_guard lk(s_mutex);
  return s_active;
}

bool Ranked()
{
  std::lock_guard lk(s_mutex);
  return s_ranked;
}

void SetOwnRating(int rating)
{
  {
    std::lock_guard lk(s_mutex);
    s_rating = rating;
  }
  PublishIdentity();
}

int OwnRating()
{
  std::lock_guard lk(s_mutex);
  return s_rating;
}

bool SoloReady()
{
  return s_solo_ready;
}

bool OnSoloCss()
{
  return s_on_solo_css;
}

void AfterRestore(bool keep_ready)
{
  if (!keep_ready)
    s_clear_ready = true;
}

void ClearReady()
{
  s_clear_ready = true;
}

bool TakeTimeoutWasMine()
{
  return s_last_timeout_mine.exchange(false);
}

void ConfirmTimeout(int confirmed, int local_port)
{
  TimeoutTracker& t = Timeouts();
  for (auto it = t.pending.begin(); it != t.pending.end() && it->first <= confirmed;
       it = t.pending.erase(it))
  {
    if ((it->second & 0x80) && !(t.last & 0x80))
    {
      const bool mine = local_port >= 0 && local_port < 2 && ((it->second >> local_port) & 1);
      s_last_timeout_mine = mine;
      NOTICE_LOG_FMT(ROLLBACK, "Queue: the ready timer ran out at frame {}: orca queue timeout {}",
                     it->first, mine ? "me" : "them");
      Orca::Status::Line(mine ? "orca queue timeout me" : "orca queue timeout them");
    }
    t.last = it->second;
    t.last_frame = it->first;
  }
}

void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports, bool alone)
{
  if (Rules::ProfileRuleset() == Rules::Ruleset::None)
    return;
  GuardMemory m(guard);
  // The player's own game after a room whose ready timer ran out on them: not ready any more (its
  // own memory, at its own alone boundary).
  if (alone && !resimulating && s_clear_ready.exchange(false))
  {
    const Rules::Header h = Rules::ReadHeader(m);
    if (h.Solo() && RegionMapped(m) && (m.Read8(READY) & 1))
    {
      m.Write8(READY, static_cast<u8>(m.Read8(READY) & ~1));
      NOTICE_LOG_FMT(ROLLBACK, "Queue: not ready after the room (the ready timer ran out on us)");
    }
  }
  const View v = ReadView(m, ports);
  const State before = ReadState(m);
  const State after = Advance(v, before, frame);
  if (after != before)
  {
    WriteState(m, after);
    if (!resimulating)
    {
      if ((after.flags & FLAG_GO) && !(before.flags & FLAG_GO))
        NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: both ready, on to the stage", frame);
      if ((after.locked & 2) && !(before.locked & 2))
      {
        NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: port 2's pick is in (character {:#x}, costume "
                       "{}, {} frames)",
                       frame, v.ports[1].character, v.ports[1].costume, after.steer);
      }
      if (!v.solo && after.ready != before.ready)
        NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: ready {:#x}", frame, after.ready);
    }
  }

  // The casual timeout for this frame (a re-run's replaces the first run's), until it is final.
  TimeoutTracker& t = Timeouts();
  const u64 resyncs = Orca::Events::Resyncs();
  if (!t.started || t.resyncs != resyncs)
  {
    t = {};
    t.started = true;
    t.resyncs = resyncs;
  }
  if (frame > t.last_frame)
  {
    t.pending[frame] = (after.flags & FLAG_TIMED_OUT) && v.queue2 && !v.solo ?
                           static_cast<u8>(0x80 | (after.timeout_who & 3)) :
                           0;
    while (t.pending.size() > 4096)
      t.pending.erase(t.pending.begin());
  }

  if (resimulating)
    return;
  // While port 2's pick is being put in: where its hand is, every 10th frame (the log).
  if (after.pick_set && !(after.locked & 2) && after.steer_start != 0 && after.steer % 10 == 0 &&
      after.steer <= STEER_LIMIT_FRAMES)
  {
    const Rollback::InputGate::Masks g = Gate(v, after);
    const CssPort& two = v.ports[1];
    NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: steering port 2 ({} frames): hand {} at {:.2f}, "
                   "{:.2f} (was {:.2f}, {:.2f}) over {:#x}{}, stick {} {}, press {:#x}",
                   frame, after.steer, two.hand_target, two.hand_x, two.hand_y, v.hand2_prev_x,
                   v.hand2_prev_y, two.character, two.placed ? " (down)" : "", g[1].steer_x,
                   g[1].steer_y, g[1].press);
  }
  int local = 0;
  for (const Events::PortInfo& p : ports)
  {
    if (!p.remote)
      local = p.port;
  }
  Local& l = L();
  // The log: where each port's hand put its token down (the queue identity's place).
  for (int p = 0; p < 2 && v.queue2 && v.css; ++p)
  {
    const CssPort& port = v.ports[p];
    Local::Hover& hover = l.hover[p];
    if (!port.placed && port.hand_target == Rules::CSS_HAND_GRID_HOLDING &&
        port.character != NO_CHARACTER)
    {
      if (hover.character != port.character)
      {
        hover.character = port.character;
        hover.first = {port.hand_x, port.hand_y};
      }
      hover.last = {port.hand_x, port.hand_y};
    }
    if (port.placed && !l.was_placed[p])
    {
      const auto middle = hover.character == port.character ? hover.Middle() : hover.last;
      NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: port {} put its token on character {:#x} costume "
                     "{} (the hand held it at {:.2f}, {:.2f}; over it from {:.2f}, {:.2f})",
                     frame, p + 1, port.character, port.costume, hover.last.first,
                     hover.last.second, hover.first.first, hover.first.second);
      if (p == 0 && solo_css_for_pick(v))
      {
        l.pick_place = middle;
        l.pick_place_character = port.character;
      }
    }
    l.was_placed[p] = port.placed;
  }
  const bool solo_css = v.queue2 && v.solo && v.css;
  // As the last alone boundary saw it: a boundary that isn't (the app's `host` or `join` waiting
  // to be taken) says nothing new.
  if (!solo_css)
    s_on_solo_css = false;
  else if (alone)
    s_on_solo_css = true;
  if (solo_css && alone)
  {
    const CssPort& own = v.ports[0];
    const bool ready = after.ready & 1;
    s_solo_ready = ready;
    Identity pick;
    pick.character = own.placed ? own.character : -1;
    pick.costume = own.costume;
    // Where the hand put it down (the middle of its way over that character).
    if (own.placed && l.pick_place_character == own.character)
    {
      pick.x = l.pick_place.first;
      pick.y = l.pick_place.second;
    }
    if (ready && (!l.printed_ready || l.printed.character != pick.character ||
                  l.printed.costume != pick.costume))
    {
      {
        std::lock_guard lk(s_mutex);
        s_pick = pick;
      }
      PublishIdentity();
      l.printed_ready = true;
      l.printed = pick;
      const std::string text = fmt::format("orca queue ready {} {} {}", v.ranked ? "ranked" : "casual",
                                           pick.character, pick.costume);
      NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: {} (the hand at {:.1f}, {:.1f})", frame, text,
                     pick.x, pick.y);
      Orca::Status::Line(text);
    }
    else if (!ready && l.printed_ready)
    {
      l.printed_ready = false;
      NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: orca queue unready", frame);
      Orca::Status::Line("orca queue unready");
    }
  }
  else if (v.queue2 && !v.solo)
  {
    // In a room: back on the player's own character select afterwards, ready is printed again.
    l.printed_ready = false;
    s_solo_ready = false;
  }
  else if (!v.queue2)
  {
    l.printed_ready = false;
    s_solo_ready = false;
  }

  // Skip: Z held 1.5 s on the room's character select (this player's own controller, as it was in
  // the frame that just ran: never a guess).
  const bool may_skip = v.queue2 && !v.solo && v.css && (v.plugged & 3) == 3 &&
                        !(after.flags & (FLAG_TIMED_OUT | FLAG_GO)) && local >= 0 && local < 2;
  if (l.skip.Step(may_skip, may_skip && (v.raw[local] & BUTTON_Z) != 0))
  {
    NOTICE_LOG_FMT(ROLLBACK, "Queue: frame {}: Z held {} frames: orca queue skip", frame,
                   l.skip.frames);
    Orca::Status::Line("orca queue skip");
  }
  Lines(v, after, ports, local);
}

Masks GateFrame(const Core::CPUThreadGuard& guard)
{
  GuardMemory m(guard);
  const Rules::Header h = Rules::ReadHeader(m);
  if (!h.Queue2())
    return {};
  // The ports' values aren't memory: port 2's pick for the gate is the region's copy (PICK).
  const View v = ReadView(m, {});
  return Gate(v, ReadState(m));
}

bool OwnsStart(const Core::CPUThreadGuard& guard)
{
  GuardMemory m(guard);
  const Rules::Header h = Rules::ReadHeader(m);
  return h.Queue2() && !h.Solo() && ReadSceneName(m) == "scSelctCharacter";
}

void LatchFrame(const Core::CPUThreadGuard& guard,
                const std::array<std::optional<GCPadStatus>, 4>& raw)
{
  if (Rules::ProfileRuleset() == Rules::Ruleset::None)
    return;
  GuardMemory m(guard);
  Latch(m, raw);
}
}  // namespace Orca::UX::Queue
