// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <climits>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <vector>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <picojson.h>

#include "Common/HttpRequest.h"
#include "Common/WebSocket.h"

#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Session/Session.h"
#include "Core/Orca/Session/YouGameRoom.h"

using namespace Orca::Net;

namespace
{
u64 Mix(u64 h, u64 v)
{
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h * 0xff51afd7ed558ccdull;
}

// A deterministic "game": its state is a hash of every frame number and every pad it ran with.
class FakeGame final : public Game
{
public:
  bool Save(int frame) override
  {
    // Saving the same frame twice between loads is wasted work (a full snapshot in Dolphin).
    if (!m_saved_since_load.insert(frame).second)
      ++m_double_saves;
    ++m_saves;
    m_snapshots[frame] = m_state;
    return true;
  }
  bool Load(int frame) override
  {
    m_saved_since_load.clear();
    const auto it = m_snapshots.find(frame);
    if (it == m_snapshots.end())
      return false;
    m_state = it->second;
    m_loads.emplace_back(frame, m_state);
    return true;
  }
  void SetPads(int frame, const Pads& pads) override
  {
    m_history[frame] = m_state;  // the state at the start of `frame` (re-runs overwrite)
    m_ran_pads[frame] = pads;
    m_pads_frame = frame;
    m_pads = pads;
  }
  void SetResimulating(bool resimulating) override { m_resimulating = resimulating; }
  std::optional<u64> SnapshotChecksum(int frame) override
  {
    const auto it = m_snapshots.find(frame);
    if (it == m_snapshots.end())
      return std::nullopt;
    return it->second;
  }
  void ResetPacing() override { ++m_pacing_resets; }

  // Runs the frame whose pads were set last; returns its number.
  int Run()
  {
    u64 h = Mix(m_state, static_cast<u64>(m_pads_frame));
    for (const Pad& pad : m_pads)
      for (u8 b : pad)
        h = Mix(h, b);
    m_state = h;
    // A bug that corrupts this frame on every run, re-runs included.
    if (m_corrupt_at == m_pads_frame)
      m_state ^= 1;
    return m_pads_frame;
  }

  std::map<int, u64> m_snapshots;
  std::set<int> m_saved_since_load;
  int m_saves = 0;
  int m_double_saves = 0;
  std::vector<std::pair<int, u64>> m_loads;
  std::map<int, u64> m_history;
  std::map<int, Pads> m_ran_pads;
  u64 m_state = 1;
  int m_pads_frame = -1;
  Pads m_pads{};
  bool m_resimulating = false;
  int m_pacing_resets = 0;
  std::optional<int> m_corrupt_at;
};

// Both directions of a link, with latency and jitter counted in ticks (one tick = one frame).
struct FakeNet
{
  struct InFlight
  {
    int deliver_at;
    Packet packet;
  };
  std::deque<InFlight> queues[2];
  int now = 0;
  int latency = 0;
  int jitter = 0;
  // Extra latency on the way to each end (index: the receiving end).
  std::array<int, 2> extra{0, 0};
  std::mt19937 rng{1234};

  class End final : public Transport
  {
  public:
    End(FakeNet& net, int index) : m_net(net), m_index(index) {}
    void Send(const Packet& packet) override
    {
      int delay = m_net.latency + m_net.extra[1 - m_index];
      if (m_net.jitter > 0)
        delay += std::uniform_int_distribution<int>(0, m_net.jitter)(m_net.rng);
      // A relayed WebSocket is ordered: never deliver before an earlier packet.
      auto& queue = m_net.queues[1 - m_index];
      int at = m_net.now + delay;
      if (!queue.empty())
        at = std::max(at, queue.back().deliver_at);
      queue.push_back({at, packet});
    }
    std::vector<Packet> Receive() override
    {
      std::vector<Packet> out;
      auto& queue = m_net.queues[m_index];
      while (!queue.empty() && queue.front().deliver_at <= m_net.now)
      {
        out.push_back(queue.front().packet);
        queue.pop_front();
      }
      return out;
    }
    bool Connected() const override { return true; }

  private:
    FakeNet& m_net;
    int m_index;
  };
};

struct Player
{
  Player(const Config& config, FakeNet& net, int index)
      : transport(net, index), session(config, game, transport), seat(index)
  {
  }

  // Inputs that change every few frames, differently per seat, so guesses are often wrong. With
  // tag_ticks every sample is unique (its tick is in the pad), so a test can tell which sample
  // each frame carries.
  Pad LocalPad(int tick) const
  {
    Pad pad{};
    const int phase = tick + seat * 37;
    pad[0] = static_cast<u8>((phase / 7) % 4);
    pad[1] = static_cast<u8>((phase / 11) % 3 == 0 ? 0x80 : 0);
    if (tag_ticks)
    {
      pad[2] = static_cast<u8>(tick & 0xff);
      pad[3] = static_cast<u8>(tick >> 8);
      pad[4] = 1;
    }
    return pad;
  }

  // An independent model of which sample each frame must carry: the sample at frame F goes to
  // F + delay; frames a raise skips repeat the previous input; a sample whose frame already has
  // one (after a lower, or a repeated stalled boundary) is dropped.
  void Expect(const Pad& pad, int input_delay, int frame, int delay)
  {
    if (expected.empty())
    {
      for (int f = 0; f < input_delay; ++f)
        expected[f] = Pad{};
    }
    const int target = frame + delay;
    const int newest = expected.empty() ? -1 : expected.rbegin()->first;
    if (target <= newest)
    {
      if (delay < last_delay)
        ++dropped_after_lower;
      last_delay = delay;
      return;
    }
    if (target > newest + 1 && newest >= 0)
      ++filled;
    const Pad held = newest >= 0 ? expected.rbegin()->second : Pad{};
    for (int f = newest + 1; f < target; ++f)
      expected[f] = held;
    expected[target] = pad;
    last_delay = delay;
  }

  // The tick a tagged pad was sampled at (-1: neutral, never sampled).
  static int TickOf(const Pad& pad) { return pad[4] ? pad[2] | pad[3] << 8 : -1; }

  // One tick of wall time: re-run any frames a rollback needs, then run at most one new frame.
  void Tick(int tick)
  {
    // The model applies to the tick's first boundary, which samples unless it is a Wait.
    const int frame = session.CurrentFrame();
    const int delay = session.Delay();
    for (int guard = 0; guard < 64; ++guard)
    {
      const Step step = session.OnBoundary(completed, LocalPad(tick));
      if (guard == 0)
      {
        first_steps.push_back({tick, frame, step.kind});
        if (model_input_delay >= 0 && step.kind != StepKind::Wait)
          Expect(LocalPad(tick), model_input_delay, frame, delay);
      }
      if (step.kind == StepKind::Ended || step.kind == StepKind::Stall ||
          step.kind == StepKind::Wait)
        return;
      const bool new_frame = step.kind == StepKind::Run && !session.Resimulating();
      completed = game.Run();
      if (new_frame)
        return;
    }
    FAIL() << "too many boundaries in one tick";
  }

  FakeGame game;
  FakeNet::End transport;
  Session session;
  int seat;
  int completed = -1;
  bool tag_ticks = false;
  // >= 0: track `expected` (the session's input_delay).
  int model_input_delay = -1;
  std::map<int, Pad> expected;
  int last_delay = -1;
  int filled = 0;
  int dropped_after_lower = 0;
  struct First
  {
    int tick;
    int frame;
    StepKind kind;
  };
  // The first boundary of every tick: the frame it was at and what it returned.
  std::vector<First> first_steps;
};

struct Match
{
  explicit Match(Config config, int latency, int jitter)
  {
    net.latency = latency;
    net.jitter = jitter;
    config.local_seat = 0;
    a = std::make_unique<Player>(config, net, 0);
    config.local_seat = 1;
    b = std::make_unique<Player>(config, net, 1);
  }
  void Play(int ticks)
  {
    const int end = played + ticks;
    for (int t = played; t < end; ++t)
    {
      net.now = t;
      // Both ends measure their round trip to the relay every half second. Through a relay
      // halfway, that is the link's one-way latency, unless a test gives each side its own.
      if (rtt_samples && t % 30 == 0)
      {
        for (int i = 0; i < 2; ++i)
        {
          int rtt = net.latency;
          if (net.jitter > 0)
            rtt += std::uniform_int_distribution<int>(0, net.jitter)(rtt_rng);
          (i == 0 ? a : b)->session.OnRoundTrip(relay_rtt_ms[i] >= 0 ? relay_rtt_ms[i] :
                                                                        rtt * 1000 / 60);
        }
      }
      a->Tick(t);
      b->Tick(t);
      // A machine whose clock runs fast: one extra frame every `a_extra_every` ticks.
      if (a_extra_every > 0 && t % a_extra_every == a_extra_every - 1)
        a->Tick(t);
    }
    played = end;
  }
  // Frames both players have confirmed must have identical saved states.
  int CompareConfirmed() const
  {
    const int confirmed = std::min(a->session.GetStats().confirmed_frame,
                                   b->session.GetStats().confirmed_frame);
    int compared = 0;
    for (const auto& [frame, state] : a->game.m_history)
    {
      if (frame > confirmed)
        continue;
      const auto other = b->game.m_history.find(frame);
      if (other == b->game.m_history.end())
        continue;
      EXPECT_EQ(state, other->second) << "frame " << frame;
      ++compared;
    }
    return compared;
  }

  // Each frame both players confirmed must carry, on the other side, the sample the model expects.
  // Returns the frames checked.
  static int CheckSamples(const Player& from, const Player& to)
  {
    const int confirmed = to.session.GetStats().confirmed_frame;
    int checked = 0;
    for (const auto& [frame, pad] : from.expected)
    {
      if (frame > confirmed)
        break;
      const auto ran = to.game.m_ran_pads.find(frame);
      if (ran == to.game.m_ran_pads.end())
        continue;
      EXPECT_EQ(ran->second[from.seat], pad) << "seat " << from.seat << " frame " << frame;
      ++checked;
    }
    return checked;
  }

  FakeNet net;
  std::unique_ptr<Player> a, b;
  bool rtt_samples = false;
  std::array<int, 2> relay_rtt_ms{-1, -1};
  std::mt19937 rtt_rng{99};
  int a_extra_every = 0;
  int played = 0;
};

// The newest frame this player ran with confirmed inputs (confirmed_frame may run ahead of it).
int Settled(const Player& p)
{
  return std::min(p.session.GetStats().confirmed_frame, p.session.CurrentFrame() - 1);
}

void ExpectClean(const Match& match)
{
  for (const Player* p : {match.a.get(), match.b.get()})
    EXPECT_TRUE(p->session.Error().empty()) << p->session.Error();
}

// A match through YouGame's relay in wall-clock time (microseconds), as two Orca processes play it.
// Each side paces frames on its own clock like Dolphin's throttle: a stall or a wait restarts
// pacing from then, the time a rollback's re-run takes is made up afterwards, and a side more than
// MaxFallback (100 ms) behind relaxes. Like SessionPort.cpp's StepSession, a stalled boundary is
// polled every millisecond and a time-sync wait holds one frame period. Like YouGameRoom.cpp, the
// room thread coalesces packets (CoalescePacket) and sends the newest at most every 10 ms, pings
// the relay every second, and with a test delay holds every message it sends and receives
// (held_out, held_in). Packets travel as their JSON (EncodePacket, DecodePacket).
class RelayMatch
{
public:
  using Micros = s64;
  static constexpr Micros MS = 1000;
  static constexpr Micros FRAME_US = 16667;

  struct Side
  {
    // The test delay this side's room holds each message it sends and receives, plus 0..jitter_ms
    // per message (never overtaking the message before it).
    int hold_out_ms = 0;
    int hold_in_ms = 0;
    int jitter_out_ms = 0;
    int jitter_in_ms = 0;
    // One way between this machine and the relay.
    int relay_ms = 8;
    // Frames per second relative to 60 (1.01: a clock 1% fast).
    double speed = 1.0;
    // Host time one re-run frame takes.
    Micros rerun_us = 0;
    // The player changes its input about once every this many frames.
    int input_every = 7;
    // The room thread's least time between two sends (YouGameRoom's SEND_GAP).
    int send_gap_ms = 10;
    // Host hitches (a disc read, a shader compile): the emulator stops for 5-20 frames, about once
    // every hitch_every_s seconds (0: never), plus hitch_burst more in the first ten seconds (boot
    // and menus). Each slow frame is reported to the session like OnlineMatch does.
    int hitch_every_s = 0;
    int hitch_burst = 0;
    bool report_hitches = true;
  };

  RelayMatch(Config config, const Side& a, const Side& b)
  {
    for (int i = 0; i < 2; ++i)
    {
      config.local_seat = i;
      m_peers[i] = std::make_unique<Peer>(config, i == 0 ? a : b, i);
    }
  }

  // Runs until both sides have run `frames` frames (or ten minutes of wall time pass).
  void Play(int frames)
  {
    for (; m_now < 600'000 * MS; m_now += MS)
    {
      if (At(0).session.CurrentFrame() >= frames && At(1).session.CurrentFrame() >= frames)
        return;
      if (::testing::Test::HasFatalFailure())
        return;
      MoveMessages();
      for (int i = 0; i < 2; ++i)
      {
        RoomThread(i);
        Emulate(i);
      }
    }
  }

  const Session& SessionOf(int i) const { return m_peers[i]->session; }
  const FakeGame& GameOf(int i) const { return m_peers[i]->game; }
  int HitchesOf(int i) const { return m_peers[i]->hitches_reported; }

  int CompareConfirmed() const
  {
    const int confirmed = std::min(SessionOf(0).GetStats().confirmed_frame,
                                   SessionOf(1).GetStats().confirmed_frame);
    int compared = 0;
    for (const auto& [frame, state] : GameOf(0).m_history)
    {
      const auto other = GameOf(1).m_history.find(frame);
      if (frame > confirmed || other == GameOf(1).m_history.end())
        continue;
      EXPECT_EQ(state, other->second) << "frame " << frame;
      ++compared;
    }
    return compared;
  }

  void ExpectClean() const
  {
    for (int i = 0; i < 2; ++i)
      EXPECT_TRUE(SessionOf(i).Error().empty()) << SessionOf(i).Error();
  }

  // One line per side, for the record: what a test's numbers were.
  void Print(const char* name) const
  {
    for (int i = 0; i < 2; ++i)
    {
      const Stats& s = SessionOf(i).GetStats();
      std::printf("[ %s %c ] rollbacks %d, re-run %d, deepest %d, stalls %d, waits %d, delay %d "
                  "(+%d -%d reverted %d), advantage %.2f, round trip %d ms, hitches %d\n",
                  name, 'A' + i, s.rollbacks, s.resimulated_frames, s.deepest_rollback, s.stalls,
                  s.waits, s.delay, s.delay_raises, s.delay_lowers, s.delay_reverts,
                  s.frame_advantage, m_peers[i]->rtt, m_peers[i]->hitches_reported);
    }
  }

private:
  struct Wire
  {
    Micros due;
    int from;
    int to;  // == from: a ping, back from the relay as a pong
    Micros ping_sent;
    std::string text;
  };

  struct Link final : Transport
  {
    void Send(const Packet& packet) override { CoalescePacket(&outgoing, packet); }
    std::vector<Packet> Receive() override { return std::exchange(inbox, {}); }
    bool Connected() const override { return true; }
    std::optional<Packet> outgoing;
    std::vector<Packet> inbox;
  };

  struct Peer
  {
    Peer(const Config& config, const Side& s, int index)
        : side(s), session(config, game, link), seat(index),
          period(static_cast<Micros>(std::llround(FRAME_US / s.speed))),
          start(static_cast<Micros>(s.relay_ms + s.hold_in_ms) * MS)
    {
      // The room's start message reaches each machine after its own way from the relay.
      next_call = start;
      pace = start;
      std::mt19937 rng(77 + index);
      const auto hitch = [&rng](Micros at) {
        return std::pair{at, std::uniform_int_distribution<int>(5, 20)(rng) * FRAME_US};
      };
      for (int i = 0; i < s.hitch_burst; ++i)
      {
        const Micros at = start + std::uniform_int_distribution<Micros>(0, 10'000 * MS)(rng);
        hitches.push_back(hitch(at));
      }
      for (Micros at = start; s.hitch_every_s > 0 && at < 600'000 * MS;)
      {
        at += std::uniform_int_distribution<Micros>(s.hitch_every_s * 500 * MS,
                                                    s.hitch_every_s * 1500 * MS)(rng);
        hitches.push_back(hitch(at));
      }
      std::sort(hitches.begin(), hitches.end());
    }
    Side side;
    FakeGame game;
    Link link;
    Session session;
    int seat;
    Micros period;
    Micros start;
    int completed = -1;
    // When the emulator next calls the session, and when the current boundary was due.
    Micros next_call;
    Micros pace;
    // Inside one boundary's StepSession loop (stall polls, a wait).
    bool in_step = false;
    bool waited = false;
    int resets_before = 0;
    Micros last_send = -1'000'000 * MS;
    Micros last_ping = -1'000'000 * MS;
    int rtt = -1;
    u32 rtt_sequence = 0;
    u32 fed_sequence = 0;
    std::deque<Wire> held_out, uplink, downlink, held_in;
    // When each hitch starts and how long it stops the emulator; the next one; when the last
    // frame's boundary returned (OnlineMatch times frames from there, so stalls and waits don't
    // count).
    std::vector<std::pair<Micros, Micros>> hitches;
    size_t next_hitch = 0;
    std::optional<Micros> last_return;
    int hitches_reported = 0;
  };

  Peer& At(int i) { return *m_peers[i]; }

  Micros Hold(int ms, int jitter_ms)
  {
    const int extra = jitter_ms > 0 ? std::uniform_int_distribution<int>(0, jitter_ms)(m_rng) : 0;
    return static_cast<Micros>(ms + extra) * MS;
  }

  static void Enter(std::deque<Wire>& queue, Wire wire, Micros due)
  {
    wire.due = queue.empty() ? due : std::max(due, queue.back().due);
    queue.push_back(std::move(wire));
  }

  void MoveMessages()
  {
    const auto due = [this](std::deque<Wire>& q) { return !q.empty() && q.front().due <= m_now; };
    const auto pop = [](std::deque<Wire>& q) {
      Wire w = std::move(q.front());
      q.pop_front();
      return w;
    };
    for (int i = 0; i < 2; ++i)
    {
      while (due(At(i).held_out))
        Enter(At(i).uplink, pop(At(i).held_out), m_now + At(i).side.relay_ms * MS);
    }
    for (int i = 0; i < 2; ++i)
    {
      while (due(At(i).uplink))
      {
        Wire w = pop(At(i).uplink);
        const int to = w.to;
        Enter(At(to).downlink, std::move(w), m_now + At(to).side.relay_ms * MS);
      }
    }
    for (int i = 0; i < 2; ++i)
    {
      Peer& p = At(i);
      while (due(p.downlink))
        Enter(p.held_in, pop(p.downlink), m_now + Hold(p.side.hold_in_ms, p.side.jitter_in_ms));
      while (due(p.held_in))
      {
        const Wire w = pop(p.held_in);
        if (w.from == i)
        {
          p.rtt = static_cast<int>((m_now - w.ping_sent) / MS);
          ++p.rtt_sequence;
          continue;
        }
        Packet packet;
        ASSERT_TRUE(DecodePacket(w.text, &packet)) << w.text;
        p.link.inbox.push_back(std::move(packet));
      }
    }
  }

  void RoomThread(int i)
  {
    Peer& p = At(i);
    if (m_now - p.last_ping >= 1000 * MS)
    {
      p.last_ping = m_now;
      Enter(p.held_out, {0, i, i, m_now, {}},
            m_now + Hold(p.side.hold_out_ms, p.side.jitter_out_ms));
    }
    if (p.link.outgoing && m_now - p.last_send >= p.side.send_gap_ms * MS)
    {
      p.last_send = m_now;
      Enter(p.held_out, {0, i, 1 - i, 0, EncodePacket(*p.link.outgoing)},
            m_now + Hold(p.side.hold_out_ms, p.side.jitter_out_ms));
      p.link.outgoing.reset();
    }
  }

  void Emulate(int i)
  {
    Peer& p = At(i);
    for (int guard = 0; p.next_call <= m_now; ++guard)
    {
      ASSERT_LT(guard, 64) << "side " << i << " ran away";
      const Micros at = p.next_call;
      if (!p.in_step)
      {
        // OnlineMatch: a new round trip is fed once per boundary, before StepSession.
        p.in_step = true;
        p.waited = false;
        p.resets_before = p.game.m_pacing_resets;
        if (p.side.report_hitches && p.last_return && at - *p.last_return > 2 * FRAME_US)
        {
          p.session.OnLocalHitch();
          ++p.hitches_reported;
        }
        if (p.rtt >= 0 && p.rtt_sequence != p.fed_sequence)
        {
          p.fed_sequence = p.rtt_sequence;
          p.session.OnRoundTrip(p.rtt);
        }
      }
      // A player's inputs follow the wall clock, changing every few frames.
      const int tick = static_cast<int>(at / FRAME_US) + i * 37;
      const int every = p.side.input_every;
      Pad pad{};
      pad[0] = static_cast<u8>((tick / every) % 4);
      pad[1] = static_cast<u8>((tick * 7 / (every * 11)) % 3 == 0 ? 0x80 : 0);
      const Step step = p.session.OnBoundary(p.completed, pad);
      if (step.kind == StepKind::Wait)
      {
        p.waited = true;
        p.next_call = at + FRAME_US;
        continue;
      }
      if (step.kind == StepKind::Stall)
      {
        p.next_call = at + MS;
        continue;
      }
      if (step.kind == StepKind::Ended)
      {
        p.next_call = std::numeric_limits<Micros>::max();
        return;
      }
      p.in_step = false;
      p.last_return = at;
      const bool reset = p.waited || p.game.m_pacing_resets != p.resets_before;
      p.completed = p.game.Run();
      if (p.session.Resimulating())
      {
        p.next_call = at + p.side.rerun_us;
        continue;
      }
      p.pace = std::max((reset ? at : p.pace) + p.period, at - 100 * MS);
      p.next_call = std::max(p.pace, at);
      // A hitch: this frame takes that much longer; the throttle makes up what it can afterwards.
      if (p.next_hitch < p.hitches.size() && p.hitches[p.next_hitch].first <= at)
        p.next_call += p.hitches[p.next_hitch++].second;
    }
  }

  std::array<std::unique_ptr<Peer>, 2> m_peers;
  Micros m_now = -3000 * MS;  // the lobby: pings before the match starts
  std::mt19937 m_rng{4321};
};
}  // namespace

TEST(OrcaSession, NoLatencyNeverRollsBack)
{
  Match match(Config{}, 0, 0);
  match.Play(600);
  EXPECT_TRUE(match.a->session.Error().empty()) << match.a->session.Error();
  EXPECT_TRUE(match.b->session.Error().empty()) << match.b->session.Error();
  EXPECT_EQ(match.a->session.GetStats().rollbacks, 0);
  EXPECT_EQ(match.b->session.GetStats().rollbacks, 0);
  EXPECT_GT(match.CompareConfirmed(), 500);
  EXPECT_GT(match.a->session.GetStats().checksums_matched, 5);
}

TEST(OrcaSession, SavesOnlyFramesItMayRollBackTo)
{
  // Remote inputs arrive within the input delay, so no frame runs on a guess: only checksum frames
  // (every 60) need a snapshot.
  Match match(Config{}, 1, 0);
  match.Play(600);
  EXPECT_TRUE(match.a->session.Error().empty()) << match.a->session.Error();
  EXPECT_EQ(match.a->session.GetStats().rollbacks, 0);
  EXPECT_LE(match.a->game.m_snapshots.size(), 20u);
  EXPECT_GT(match.a->session.GetStats().checksums_matched, 5);
}

TEST(OrcaSession, LatencyRollsBackAndStaysInSync)
{
  Match match(Config{}, 4, 3);
  match.Play(1200);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_TRUE(p->session.Error().empty()) << p->session.Error();
    EXPECT_GT(p->session.GetStats().rollbacks, 10);
    EXPECT_LE(p->session.GetStats().deepest_rollback, Config{}.max_rollback);
    EXPECT_GT(p->session.GetStats().checksums_matched, 10);
  }
  EXPECT_GT(match.CompareConfirmed(), 1000);
}

TEST(OrcaSession, LatencyBeyondTheWindowStalls)
{
  Config config;
  config.max_rollback = 4;
  Match match(config, 10, 2);
  match.Play(900);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_TRUE(p->session.Error().empty()) << p->session.Error();
    EXPECT_GT(p->session.GetStats().stalls, 0);
    EXPECT_GT(p->game.m_pacing_resets, 0);
    EXPECT_LE(p->session.GetStats().deepest_rollback, config.max_rollback);
  }
  EXPECT_GT(match.CompareConfirmed(), 300);
}

TEST(OrcaSession, DetectsDesync)
{
  Match match(Config{}, 3, 1);
  match.b->game.m_corrupt_at = 100;
  match.Play(600);
  const bool detected = match.a->session.Error().find("Desync") != std::string::npos ||
                        match.b->session.Error().find("Desync") != std::string::npos;
  EXPECT_TRUE(detected) << match.a->session.Error() << " / " << match.b->session.Error();
}

TEST(OrcaPadCodec, NeutralIsZeroAndRoundTrips)
{
  EXPECT_EQ(EncodePad(GCPadStatus{}), Pad{});

  GCPadStatus status;
  status.button = PAD_BUTTON_A | PAD_TRIGGER_Z | PAD_GET_ORIGIN | PAD_USE_ORIGIN;
  status.stickX = 0;
  status.stickY = 255;
  status.substickX = 37;
  status.substickY = 200;
  status.triggerLeft = 255;
  status.triggerRight = 12;
  status.analogA = 99;
  const GCPadStatus back = DecodePad(EncodePad(status));
  EXPECT_EQ(back.button, PAD_BUTTON_A | PAD_TRIGGER_Z);  // origin bits never travel
  EXPECT_EQ(back.stickX, 0);
  EXPECT_EQ(back.stickY, 255);
  EXPECT_EQ(back.substickX, 37);
  EXPECT_EQ(back.substickY, 200);
  EXPECT_EQ(back.triggerLeft, 255);
  EXPECT_EQ(back.triggerRight, 12);
  EXPECT_EQ(back.analogA, 0);
  EXPECT_TRUE(back.isConnected);
}

TEST(OrcaSession, RejectsHostilePackets)
{
  struct Inject final : Transport
  {
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override { return std::exchange(queue, {}); }
    bool Connected() const override { return true; }
    std::vector<Packet> queue;
  };
  const auto run = [](Packet bad) {
    FakeGame game;
    Inject net;
    Config config;
    Session session(config, game, net);
    int completed = -1;
    for (int f = 0; f < 3; ++f)
    {
      session.OnBoundary(completed, Pad{});
      completed = game.Run();
    }
    bad.seat = 1;
    net.queue.push_back(bad);
    session.OnBoundary(completed, Pad{});
    return session.Error();
  };
  Packet huge_frame;
  huge_frame.first_frame = 2147483000;
  huge_frame.pads.resize(1);
  EXPECT_NE(run(huge_frame).find("invalid packet"), std::string::npos);

  Packet many_pads;
  many_pads.first_frame = 0;
  many_pads.pads.resize(100000);
  EXPECT_NE(run(many_pads).find("invalid packet"), std::string::npos);

  Packet negative;
  negative.first_frame = -5;
  negative.pads.resize(1);
  EXPECT_NE(run(negative).find("invalid packet"), std::string::npos);

  Packet fine;
  fine.first_frame = 0;
  fine.pads.resize(4);
  EXPECT_TRUE(run(fine).empty());
}

// Live check against YouGame's rooms worker, only with ORCA_LIVE_TEST=1 (writes nothing: an
// anonymous dev game whose results are never recorded).
TEST(OrcaLive, WebSocketReachesARoom)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  const std::string site = std::getenv("ORCA_SITE") ? std::getenv("ORCA_SITE") : "https://yougame.co";

  Common::HttpRequest http(std::chrono::seconds(15));
  const std::string body = R"({"dev":"orca-live-test","player_id":"orca-live-0000000001","name":"Orca","players":2,"mode":"orca-v1","room":"orcalive1",)"
                           R"("lobby":{"version":1,"minPlayers":2,"maxLocalPlayers":1,"compatibility":"orca-test"}})";
  const auto response = http.Post(site + "/api/multiplayer/ticket", body,
                                  {{"Content-Type", "application/json"}},
                                  Common::HttpRequest::AllowedReturnCodes::All);
  ASSERT_TRUE(response.has_value()) << "no response from " << site;
  const std::string text(response->begin(), response->end());
  ASSERT_EQ(http.GetLastResponseCode(), 200) << text;
  picojson::value json;
  ASSERT_TRUE(picojson::parse(json, text).empty()) << text;
  const std::string url = json.get("url").to_str();
  const std::string ticket = json.get("ticket").to_str();
  std::string ws_url = url;
  if (ws_url.starts_with("https://"))
    ws_url.replace(0, 8, "wss://");
  else if (ws_url.starts_with("http://"))
    ws_url.replace(0, 7, "ws://");
  ws_url += "/room/orcalive1?ticket=" + http.EscapeComponent(ticket);

  Common::WebSocket ws;
  std::string error;
  ASSERT_TRUE(ws.Connect(ws_url, std::chrono::seconds(10), &error)) << error;
  std::optional<std::string> first;
  for (int i = 0; i < 20 && !first; ++i)
    first = ws.ReceiveText(std::chrono::milliseconds(500));
  ASSERT_TRUE(first.has_value()) << ws.Error();
  picojson::value welcome;
  ASSERT_TRUE(picojson::parse(welcome, *first).empty()) << *first;
  EXPECT_EQ(welcome.get("t").to_str(), "welcome") << *first;
  EXPECT_TRUE(ws.SendText(R"({"t":"ping","at":1})", std::chrono::seconds(2))) << ws.Error();
  bool pong = false;
  for (int i = 0; i < 20 && !pong; ++i)
  {
    if (auto m = ws.ReceiveText(std::chrono::milliseconds(500)))
      pong = m->find("\"pong\"") != std::string::npos;
  }
  EXPECT_TRUE(pong);
  ws.Close();
}

TEST(OrcaSessionDelay, RisesUnderHeavyRollback)
{
  // Six frames of latency against a delay of two: every wrong guess re-simulates four frames or so.
  Match match(Config{}, 6, 2);
  match.Play(240);
  const int early_resim = match.a->session.GetStats().resimulated_frames;
  match.Play(720);
  const int before_late = match.a->session.GetStats().resimulated_frames;
  match.Play(240);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_GE(p->session.GetStats().delay_raises, 2);
    EXPECT_GE(p->session.Delay(), 4);
    EXPECT_LE(p->session.Delay(), Config{}.max_delay);
  }
  // The raised delay pays off: the last four seconds re-simulate less than half what the first
  // four did (not zero: up to eight frames of latency plus jitter against a ceiling of six).
  const int late_resim = match.a->session.GetStats().resimulated_frames - before_late;
  EXPECT_LT(late_resim * 2, early_resim);
  EXPECT_GT(match.CompareConfirmed(), 1000);
}

TEST(OrcaSessionDelay, StaysPutWhenQuietOrDisabled)
{
  Match quiet(Config{}, 1, 0);
  quiet.Play(1200);
  ExpectClean(quiet);
  EXPECT_EQ(quiet.a->session.Delay(), 2);
  EXPECT_EQ(quiet.a->session.GetStats().delay_raises, 0);

  Config fixed;
  fixed.max_delay = fixed.input_delay;
  Match off(fixed, 6, 2);
  off.Play(1200);
  ExpectClean(off);
  EXPECT_EQ(off.a->session.Delay(), 2);
  EXPECT_EQ(off.a->session.GetStats().delay_raises, 0);
  EXPECT_GT(off.CompareConfirmed(), 1000);
}

TEST(OrcaSessionDelay, StartsFromMeasuredLatency)
{
  // Rollback hides latency up to its window (7, less 2 for jitter): only what is beyond it goes
  // into input delay. 50 ms to the relay on both sides: 3 frames one way, all hidden.
  Match near(Config{}, 3, 0);
  near.rtt_samples = true;
  near.Play(1);
  EXPECT_EQ(near.a->session.Delay(), 2);
  EXPECT_EQ(near.b->session.Delay(), 2);

  // 150 ms each: 9 frames one way, 4 of them delay and 5 rolled back. One stall at the start (the
  // first packet is 9 frames away, the window 7), none after: the window holds the rest.
  Match far(Config{}, 9, 0);
  far.rtt_samples = true;
  far.Play(1);
  EXPECT_EQ(far.a->session.Delay(), 4);
  EXPECT_EQ(far.b->session.Delay(), 4);
  far.Play(239);
  ExpectClean(far);
  for (const Player* p : {far.a.get(), far.b.get()})
  {
    EXPECT_LE(p->session.GetStats().stalls, 1);
    EXPECT_GT(p->session.GetStats().rollbacks, 0);
    EXPECT_LE(p->session.GetStats().deepest_rollback, 5);
  }
  EXPECT_GT(far.CompareConfirmed(), 200);

  // A huge round trip is capped at max_delay; after the first seconds, round trips move nothing.
  Match huge(Config{}, 0, 0);
  huge.a->session.OnRoundTrip(1000);
  EXPECT_EQ(huge.a->session.Delay(), Config{}.max_delay);
  Match late(Config{}, 0, 0);
  late.Play(200);
  late.a->session.OnRoundTrip(1000);
  EXPECT_EQ(late.a->session.Delay(), Config{}.input_delay);
}

TEST(OrcaSessionDelay, IncreaseFillsSkippedFramesAndPeerConfirmsThem)
{
  // Every sample is unique, so every guess is wrong and rollbacks are heavy: the delay rises, and
  // each rise skips a frame that must still carry an explicit input.
  Match match(Config{}, 5, 1);
  for (Player* p : {match.a.get(), match.b.get()})
  {
    p->tag_ticks = true;
    p->model_input_delay = Config{}.input_delay;
  }
  match.Play(900);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_GE(p->session.GetStats().delay_raises, 2);
    EXPECT_EQ(p->filled, p->session.GetStats().delay_raises + p->session.GetStats().delay_reverts);
  }
  EXPECT_GT(Match::CheckSamples(*match.a, *match.b), 800);
  EXPECT_GT(Match::CheckSamples(*match.b, *match.a), 800);
  EXPECT_GT(match.CompareConfirmed(), 800);
}

TEST(OrcaSessionDelay, FallsAfterQuietTimeAndDropsOneSamplePerStep)
{
  // A measures 400 ms to the relay before the match starts, so it opens at max_delay; the link is
  // really instant, so quiet windows bring it back down one frame at a time. B stays at 2 all
  // along: the two delays differ for the whole match and the frames still agree.
  Match match(Config{}, 0, 0);
  for (Player* p : {match.a.get(), match.b.get()})
  {
    p->tag_ticks = true;
    p->model_input_delay = Config{}.input_delay;
  }
  match.a->session.OnRoundTrip(400);
  ASSERT_EQ(match.a->session.Delay(), 6);
  match.Play(1800);
  ExpectClean(match);
  const Stats& a = match.a->session.GetStats();
  EXPECT_EQ(match.a->session.Delay(), 2);
  EXPECT_EQ(a.delay_lowers, 4);
  EXPECT_EQ(a.delay_reverts, 0);
  EXPECT_EQ(a.rollbacks, 0);
  // A steady link needs five quiet seconds per step: the last lower lands after twenty.
  EXPECT_EQ(match.b->session.Delay(), 2);
  EXPECT_EQ(match.b->session.GetStats().delay_lowers, 0);
  // Exactly one sample is dropped per step down, and every frame B ran carries the sample the
  // model says (none of A's frames was skipped or doubled).
  EXPECT_EQ(match.a->dropped_after_lower, 4);
  EXPECT_GT(Match::CheckSamples(*match.a, *match.b), 1700);
  EXPECT_GT(Match::CheckSamples(*match.b, *match.a), 1700);
  EXPECT_GT(match.CompareConfirmed(), 1700);
}

TEST(OrcaSessionDelay, LowerThatBringsRollbacksBackReverts)
{
  // Four frames of latency. Both measure 150 ms to the relay before the match (a slow first ping),
  // so both open at 4, where inputs arrive just in time. A step down to 3 makes the other side roll
  // back, so it reverts, and the next try waits longer.
  Match match(Config{}, 4, 0);
  match.a->session.OnRoundTrip(150);
  match.b->session.OnRoundTrip(150);
  ASSERT_EQ(match.a->session.Delay(), 4);
  match.Play(2400);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    const Stats& stats = p->session.GetStats();
    EXPECT_GE(stats.delay_lowers, 2);
    EXPECT_EQ(stats.delay_reverts, stats.delay_lowers - (p->session.Delay() == 3 ? 1 : 0));
    EXPECT_EQ(stats.delay_raises, 0);
    // Backoff: 5, 5, 10, 20 ... quiet seconds between tries, not one every five.
    EXPECT_LE(stats.delay_lowers, 4);
  }
  EXPECT_GT(match.CompareConfirmed(), 2000);
}

TEST(OrcaSessionTimeSync, FastSideWaits)
{
  Config config;
  config.max_delay = config.input_delay;  // keep the delay out of it
  const auto play = [&config](bool time_sync, int extra_every, bool tagged = false) {
    config.time_sync = time_sync;
    auto match = std::make_unique<Match>(config, 3, 1);
    match->rtt_samples = true;
    match->a_extra_every = extra_every;
    for (Player* p : {match->a.get(), match->b.get()})
    {
      p->tag_ticks = tagged;
      p->model_input_delay = tagged ? config.input_delay : -1;
    }
    match->Play(2400);
    ExpectClean(*match);
    EXPECT_GT(match->CompareConfirmed(), 2000);
    return match;
  };
  // A's clock runs 2.5% fast (one extra frame every 40 ticks).
  const auto synced = play(true, 40);
  const auto unsynced = play(false, 40);
  const Stats& a = synced->a->session.GetStats();
  const Stats& b = synced->b->session.GetStats();
  EXPECT_GT(a.waits, 30);
  EXPECT_LE(a.waits, 2500 / 30);
  EXPECT_EQ(b.waits, 0);
  EXPECT_EQ(unsynced->a->session.GetStats().waits, 0);
  // Without time sync the fast side runs into the rollback window and stalls there, re-simulating
  // deep all the way. With it, nobody stalls, the lead stays near the one-frame threshold, and the
  // two sides together re-simulate about half as much.
  const Stats& ua = unsynced->a->session.GetStats();
  const Stats& ub = unsynced->b->session.GetStats();
  EXPECT_GT(ua.stalls, 30);
  EXPECT_EQ(a.stalls, 0);
  EXPECT_EQ(b.stalls, 0);
  EXPECT_LT(a.frame_advantage, 2.0);
  EXPECT_LT((a.resimulated_frames + b.resimulated_frames) * 3,
            (ua.resimulated_frames + ub.resimulated_frames) * 2);

  // Equal clocks: nobody waits.
  const auto even = play(true, 0);
  EXPECT_EQ(even->a->session.GetStats().waits, 0);
  EXPECT_EQ(even->b->session.GetStats().waits, 0);

  // With every sample tagged: each wait resets pacing (the frame it held is not made up), and the
  // wait boundary samples nothing, so the frame it waited on carries the pad of the boundary after
  // the wait (fresh), not one sampled before it and held. Read off what B actually ran, at the
  // fixed delay: the sample of a boundary at frame F runs at F + delay.
  const auto tagged = play(true, 40, true);
  const Player& ta = *tagged->a;
  const Player& tb = *tagged->b;
  const int delay = config.input_delay;
  EXPECT_GT(ta.session.GetStats().waits, 30);
  EXPECT_GE(ta.game.m_pacing_resets, ta.session.GetStats().waits);
  EXPECT_EQ(ta.session.GetStats().stalls, 0);
  EXPECT_GT(Match::CheckSamples(ta, tb), 2000);
  EXPECT_GT(Match::CheckSamples(tb, ta), 2000);
  int waits_checked = 0;
  for (size_t i = 0; i + 1 < ta.first_steps.size(); ++i)
  {
    const Player::First& wait = ta.first_steps[i];
    const Player::First& after = ta.first_steps[i + 1];
    if (wait.kind != StepKind::Wait || wait.frame + delay > Settled(tb))
      continue;
    ASSERT_EQ(after.frame, wait.frame);
    EXPECT_EQ(tb.game.m_ran_pads.at(wait.frame + delay)[0], ta.LocalPad(after.tick))
        << "wait at tick " << wait.tick;
    ++waits_checked;
  }
  EXPECT_GT(waits_checked, 30);
  // B never waits or stalls: its samples advance exactly one tick per frame.
  for (int f = delay + 1; f <= Settled(ta); ++f)
  {
    EXPECT_EQ(Player::TickOf(ta.game.m_ran_pads.at(f)[1]),
              Player::TickOf(ta.game.m_ran_pads.at(f - 1)[1]) + 1)
        << "frame " << f;
  }
}

TEST(OrcaSessionTimeSync, UnequalRelayRoundTrips)
{
  // A is 17 ms from the relay, B 217 ms: 7 frames apart one way. A round-trip-based estimate of
  // the other's frame would be off by (217 - 17) / 4 ms on each side in opposite directions and
  // make A wait until it sat about two frames behind. Frame numbers alone don't care.
  Config config;
  Match match(config, 7, 0);
  match.rtt_samples = true;
  match.relay_rtt_ms = {17, 217};
  match.Play(1);
  // Alone, each side assumes the other is as far from the relay as itself.
  EXPECT_EQ(match.a->session.Delay(), 2);
  EXPECT_EQ(match.b->session.Delay(), 6);
  match.Play(29);
  // Once the other's round trip arrived: (17 + 217) / 2 = 117 ms = 8 frames one way, 5 of them
  // rolled back. Both agree, and B came down from its first guess.
  EXPECT_EQ(match.a->session.Delay(), 3);
  EXPECT_EQ(match.b->session.Delay(), 3);
  match.Play(1770);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.GetStats().waits, 0);
    EXPECT_LT(std::abs(p->session.GetStats().frame_advantage), 1.0);
  }
  EXPECT_GT(match.CompareConfirmed(), 1600);
}

TEST(OrcaSession, NeverSavesAFrameTwice)
{
  // Stalls and time-sync waits repeat the boundary of the same frame many times: one snapshot each.
  Config stalling;
  stalling.max_rollback = 4;
  Match stalls(stalling, 10, 2);
  stalls.Play(900);
  Config waiting;
  waiting.max_delay = waiting.input_delay;
  Match waits(waiting, 3, 1);
  waits.rtt_samples = true;
  waits.a_extra_every = 40;
  waits.Play(1200);
  for (const Match* match : {&stalls, &waits})
  {
    ExpectClean(*match);
    for (const Player* p : {match->a.get(), match->b.get()})
    {
      EXPECT_GT(p->game.m_saves, 100);
      EXPECT_EQ(p->game.m_double_saves, 0);
    }
  }
  EXPECT_GT(stalls.a->session.GetStats().stalls, 0);
  EXPECT_GT(waits.a->session.GetStats().waits, 0);
}

TEST(OrcaSessionDelay, FixedDelaySamplesLandDelayFramesLater)
{
  // Checked against the rule itself, not a model of the session: at a fixed delay d with no
  // stall or wait, frames below d run neutral and the sample of tick t runs at frame t + d, on
  // both machines. Also with no delay at all.
  for (const int delay : {2, 0})
  {
    Config config;
    config.input_delay = delay;
    config.max_delay = delay;
    Match match(config, 1, 0);
    for (Player* p : {match.a.get(), match.b.get()})
      p->tag_ticks = true;
    match.Play(300);
    ExpectClean(match);
    for (const Player* from : {match.a.get(), match.b.get()})
    {
      const Player* to = from == match.a.get() ? match.b.get() : match.a.get();
      EXPECT_EQ(from->session.GetStats().stalls, 0);
      EXPECT_EQ(from->session.GetStats().waits, 0);
      const int confirmed = Settled(*to);
      ASSERT_GT(confirmed, 250);
      for (int f = 0; f <= confirmed; ++f)
      {
        const Pad& ran = to->game.m_ran_pads.at(f)[from->seat];
        EXPECT_EQ(ran, f < delay ? Pad{} : from->LocalPad(f - delay))
            << "delay " << delay << " seat " << from->seat << " frame " << f;
      }
    }
    EXPECT_GT(match.CompareConfirmed(), 250);
  }
}

TEST(OrcaSessionDelay, RaiseDuringAStall)
{
  // A narrow window and a slow link make A stall at the start, before B's first packet. Raising
  // A's delay while it stalls (a first round trip arriving) applies at its next sample: the
  // frames in between repeat the sample taken when the stall began.
  Config config;
  config.max_rollback = 2;  // so LatencyDelay takes the whole latency
  config.time_sync = false;
  Match match(config, 6, 0);
  for (Player* p : {match.a.get(), match.b.get()})
    p->tag_ticks = true;
  Player& a = *match.a;
  while (a.first_steps.empty() || a.first_steps.back().kind != StepKind::Stall)
  {
    match.Play(1);
    ASSERT_LT(match.played, 10);
  }
  const Player::First stall = a.first_steps.back();
  a.session.OnRoundTrip(1000);
  ASSERT_EQ(a.session.Delay(), 6);
  match.Play(1);
  const Player::First next = a.first_steps.back();
  ASSERT_EQ(next.kind, StepKind::Stall);
  ASSERT_EQ(next.frame, stall.frame);
  match.Play(300);
  ExpectClean(match);
  const auto ran = [&](int f) { return match.b->game.m_ran_pads.at(f)[0]; };
  // Sampled at delay 2 when the stall began, then at delay 6 on the next boundary.
  EXPECT_EQ(ran(stall.frame + 2), a.LocalPad(stall.tick));
  for (int f = stall.frame + 3; f < stall.frame + 6; ++f)
    EXPECT_EQ(ran(f), a.LocalPad(stall.tick)) << "frame " << f;
  EXPECT_EQ(ran(stall.frame + 6), a.LocalPad(next.tick));
  EXPECT_GT(match.CompareConfirmed(), 200);
}

TEST(OrcaSession, HostilePeerTelemetryIsBounded)
{
  // A peer that sends good inputs, ahead of time, with the worst numbers it may: a re-simulation
  // count past any window, a lead claiming it is far behind, frames at both ends of what the
  // session accepts, and an absurd round trip.
  struct Hostile final : Transport
  {
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override
    {
      Packet packet;
      packet.seat = 1;
      packet.first_frame = next;
      const int up_to = session->CurrentFrame() + 4;
      for (; next <= up_to; ++next)
        packet.pads.push_back(Pad{});
      packet.ack = {session->CurrentFrame(), -1, -1, -1};
      packet.current_frame = flip ? 0 : session->CurrentFrame() + MAX_FRAMES_AHEAD;
      flip = !flip;
      packet.resimulated = INT_MAX;
      packet.advantage = INT_MIN;
      packet.rtt = INT_MAX;
      packet.stalls = INT_MAX;
      packet.hitches = INT_MAX;
      return {packet};
    }
    bool Connected() const override { return true; }
    Session* session = nullptr;
    int next = 0;
    bool flip = false;
  };
  FakeGame game;
  Hostile net;
  Config config;
  Session session(config, game, net);
  net.session = &session;
  session.OnRoundTrip(30);
  int completed = -1;
  int frames = 0;
  for (int i = 0; i < 1200; ++i)
  {
    const Step step = session.OnBoundary(completed, Pad{});
    ASSERT_NE(step.kind, StepKind::Ended) << session.Error();
    if (step.kind == StepKind::Run || step.kind == StepKind::Rollback)
    {
      completed = game.Run();
      ++frames;
    }
  }
  const Stats& stats = session.GetStats();
  EXPECT_TRUE(session.Error().empty()) << session.Error();
  EXPECT_EQ(stats.stalls, 0);
  EXPECT_LE(stats.delay, config.max_delay);
  EXPECT_LE(session.Delay(), config.max_delay);
  EXPECT_GT(stats.waits, 0);
  EXPECT_LE(stats.waits, frames / 30 + 1);
}

TEST(OrcaSession, DepthOneRollbacksLoadTheCorrectedState)
{
  // Every remote input arrives one frame late and differs from the guess, so every frame runs
  // twice: a depth-1 rollback loads the state at the start of the frame, saved on the boundary
  // after the previous frame's own re-run. A snapshot skipped there would load the uncorrected
  // state. Checked against a game run straight with the right inputs.
  const auto remote = [](int f) { return Pad{static_cast<u8>(f % 5), static_cast<u8>(f % 3)}; };
  struct Late final : Transport
  {
    explicit Late(std::function<Pad(int)> remote_pad) : pad(std::move(remote_pad)) {}
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override
    {
      Packet packet;
      packet.seat = 1;
      packet.first_frame = next;
      for (; next < session->CurrentFrame(); ++next)
        packet.pads.push_back(pad(next));
      packet.current_frame = session->CurrentFrame();
      return {packet};
    }
    bool Connected() const override { return true; }
    std::function<Pad(int)> pad;
    Session* session = nullptr;
    int next = 0;
  };
  FakeGame game;
  Late net(remote);
  Config config;
  config.time_sync = false;
  Session session(config, game, net);
  net.session = &session;
  int completed = -1;
  for (int i = 0; i < 1000; ++i)
  {
    const Step step = session.OnBoundary(completed, Pad{});
    ASSERT_NE(step.kind, StepKind::Ended) << session.Error();
    if (step.kind == StepKind::Run || step.kind == StepKind::Rollback)
      completed = game.Run();
  }
  const Stats& stats = session.GetStats();
  EXPECT_GT(stats.rollbacks, 300);
  EXPECT_EQ(stats.deepest_rollback, 1);

  FakeGame straight;
  for (int f = 0; f <= stats.confirmed_frame; ++f)
  {
    straight.SetPads(f, Pads{Pad{}, remote(f)});
    straight.Run();
  }
  for (const auto& [frame, state] : game.m_loads)
  {
    if (frame <= stats.confirmed_frame)
      EXPECT_EQ(state, straight.m_history.at(frame)) << "load of frame " << frame;
  }
  int compared = 0;
  for (int f = 0; f <= stats.confirmed_frame; ++f, ++compared)
    EXPECT_EQ(game.m_history.at(f), straight.m_history.at(f)) << "frame " << f;
  EXPECT_GT(compared, 300);
}

namespace
{
// A burst of B's packets reaching A ten frames late against a margin of eight: A stalls once,
// briefly (a longer one leaves A behind, and B's rollbacks while time sync catches up would raise
// the delay on their own).
// With `hitch`, B's emulator froze instead, and B reports it (Session::OnLocalHitch).
void LateBurst(Match& match, bool hitch = false)
{
  if (hitch)
    match.b->session.OnLocalHitch();
  match.net.extra[0] = 10;
  match.Play(1);
  match.net.extra[0] = 0;
}
}  // namespace

TEST(OrcaSessionDelay, StallsRaiseOnARateThenDecay)
{
  // After the warmup, one late burst stalls A once: a one-off is a hitch as often as the link, and
  // one frame of delay would not cover it, so nothing moves. Two in consecutive seconds do: both
  // sides raise (B on A's report; B's delay is the one that keeps A from stalling).
  Match match(Config{}, 1, 0);
  match.Play(700);
  LateBurst(match);
  match.Play(299);
  ExpectClean(match);
  EXPECT_EQ(match.a->session.GetStats().stalls, 1);
  EXPECT_EQ(match.b->session.GetStats().stalls, 0);
  for (const Player* p : {match.a.get(), match.b.get()})
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);

  LateBurst(match);
  match.Play(59);
  LateBurst(match);
  match.Play(119);
  EXPECT_EQ(match.a->session.GetStats().stalls, 3);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 3);
    EXPECT_EQ(p->session.GetStats().delay_raises, 1);
  }
  // Then quiet: a short hold, and the ordinary quiet-window rule brings it back down.
  match.Play(60 * 6);
  for (const Player* p : {match.a.get(), match.b.get()})
    EXPECT_EQ(p->session.Delay(), 3);
  match.Play(60 * 8);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(p->session.GetStats().delay_lowers, 1);
    EXPECT_EQ(p->session.GetStats().delay_reverts, 0);
  }
  EXPECT_GT(match.CompareConfirmed(), 1500);
}

TEST(OrcaSessionDelay, OneHiccupBothWaysRaisesNothing)
{
  // The relay holds both directions at once: each side stalls once, and hears of the other's. That
  // is one event, not two stalls in a row.
  Match match(Config{}, 1, 0);
  match.Play(700);
  match.net.extra = {10, 10};
  match.Play(1);
  match.net.extra = {0, 0};
  match.Play(299);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.GetStats().stalls, 1);
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
  }
}

TEST(OrcaSessionDelay, StallsOnAPeerHitchRaiseNothing)
{
  // The same stalls in consecutive seconds, each lined up with a hitch B reports: no delay cures
  // a frozen emulator, so neither side raises.
  Match match(Config{}, 1, 0);
  match.Play(700);
  for (int i = 0; i < 4; ++i)
  {
    LateBurst(match, true);
    match.Play(59);
  }
  match.Play(120);
  ExpectClean(match);
  EXPECT_EQ(match.a->session.GetStats().stalls, 4);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
  }
  EXPECT_GT(match.CompareConfirmed(), 800);
}

TEST(OrcaSessionDelay, WarmupStallsNeedAHigherRate)
{
  // In the first ten seconds (boot, menus), two stalls a second apart raise nothing (after the
  // warmup they would); a third within three seconds still does, as a link too slow would make.
  for (const int bursts : {2, 3})
  {
    Match match(Config{}, 1, 0);
    match.Play(200);
    for (int i = 0; i < bursts; ++i)
    {
      LateBurst(match);
      match.Play(59);
    }
    match.Play(120);
    ExpectClean(match);
    EXPECT_EQ(match.a->session.GetStats().stalls, bursts);
    for (const Player* p : {match.a.get(), match.b.get()})
      EXPECT_EQ(p->session.GetStats().delay_raises, bursts == 3 ? 1 : 0) << "bursts " << bursts;
  }
}

TEST(OrcaSessionDelay, RoundTripsFloorQuietLowers)
{
  // Both measure 150 ms to the relay all match long (9 frames one way, 4 of them delay), while
  // this link is really one frame: no rollbacks, so every window is quiet. After the start, the
  // estimate still bounds how low quiet windows take the delay. It raises nothing by itself.
  Match match(Config{}, 1, 0);
  match.rtt_samples = true;
  match.relay_rtt_ms = {150, 150};
  match.Play(60 * 60);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 4);
    EXPECT_EQ(p->session.GetStats().delay_lowers, 0);
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
  }
  EXPECT_GT(match.CompareConfirmed(), 3500);
}

TEST(OrcaSessionDelay, StallBeforeFirstInputRaisesNothing)
{
  // Eight frames each way against a window of seven: both sides stall once at the start, waiting
  // for the other's first packet. That is the trip, not a delay too short (input_delay 6 leaves two
  // frames for rollback after it), so neither raises.
  Config config;
  config.input_delay = 6;
  config.max_delay = 7;
  Match match(config, 8, 0);
  match.Play(600);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.GetStats().stalls, 1);
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
    EXPECT_EQ(p->session.Delay(), 6);
  }
  EXPECT_GT(match.CompareConfirmed(), 500);
}

namespace
{
// The shape of T3 (two Orca processes over YouGame's relay, 12,000 frames): A's room holds every
// message 60 ms each way, so A and B are about 70 ms apart one way, and A's round trips to the
// relay wandered from 133 to 202 ms; B's room adds nothing (16 ms). Here A's way in from the relay
// carries 0-70 ms of jitter on top, A's clock runs 1% fast, and inputs change about every 40
// frames (T3's rate of rollbacks).
RelayMatch::Side T3SideA()
{
  RelayMatch::Side a;
  a.hold_out_ms = 60;
  a.hold_in_ms = 60;
  a.jitter_in_ms = 70;
  a.speed = 1.01;
  a.input_every = 40;
  return a;
}

RelayMatch::Side T3SideB()
{
  RelayMatch::Side b;
  b.input_every = 40;
  return b;
}
}  // namespace

TEST(OrcaSessionRelay, SlowJitteryLinkStallsRarely)
{
  // Before stalls counted from both sides, this stalled A 124 times (T3: 97): each late burst on
  // A's way in outran a margin of about 7 + B's delay (2) - 6 frames of transit - A's lead (up to a
  // frame, time sync's tolerance), so under 30 ms. A raised its own delay to 6, which could not
  // help it, and B, whose delay could, never heard of A's stalls and stayed at 2.
  RelayMatch match(Config{}, T3SideA(), T3SideB());
  match.Play(12000);
  match.Print("T3");
  match.ExpectClean();
  const Stats& a = match.SessionOf(0).GetStats();
  const Stats& b = match.SessionOf(1).GetStats();
  EXPECT_LE(a.stalls, 5);
  EXPECT_LE(b.stalls, 5);
  // B raised its delay on A's stalls, and the two delays end together.
  EXPECT_GE(b.delay_raises, 1);
  EXPECT_GE(match.SessionOf(1).Delay(), 3);
  EXPECT_LE(std::abs(match.SessionOf(0).Delay() - match.SessionOf(1).Delay()), 1);
  EXPECT_GT(match.CompareConfirmed(), 11000);
}

TEST(OrcaSessionRelay, JitterBothWaysEitherSideFast)
{
  // The same link with A's jitter on both ways, and either clock fast: the fast side runs up to a
  // frame ahead and is the one that stalls. Before stalls counted from both sides, A stalled 654
  // times when its clock was the fast one, raising only its own delay (to 6) while B's stayed at 2.
  // Raising on a rate (two in three seconds) rather than on every stall costs a few more.
  for (const bool a_fast : {true, false})
  {
    RelayMatch::Side a = T3SideA();
    RelayMatch::Side b = T3SideB();
    a.jitter_out_ms = 70;
    a.speed = a_fast ? 1.01 : 1.0;
    b.speed = a_fast ? 1.0 : 1.01;
    RelayMatch match(Config{}, a, b);
    match.Play(12000);
    match.Print(a_fast ? "both ways, A fast" : "both ways, B fast");
    match.ExpectClean();
    for (int i = 0; i < 2; ++i)
      EXPECT_LE(match.SessionOf(i).GetStats().stalls, 10) << "a_fast " << a_fast << " side " << i;
    EXPECT_GT(match.CompareConfirmed(), 11000);
  }
}

TEST(OrcaSessionRelay, LanWithHitchesTracksTheLink)
{
  // 28 ms apart, but both emulators hitch (B, a Metal machine compiling shaders, twice as often),
  // plus a burst of four each while booting. Every hitch longer than the rollback window stalls the
  // other side, and no delay would have saved it. Before, any stall raised both sides and held
  // 30 s: both delays climbed to 6, 100 ms of input lag on a LAN. Hitches reported and left out,
  // the delay stays on the link. Unreported, the same hitches still ratchet it (a control: these
  // stalls are frequent enough to pass for the link's).
  RelayMatch::Side a, b;
  a.relay_ms = b.relay_ms = 14;
  a.speed = 1.01;
  a.input_every = b.input_every = 40;
  a.hitch_every_s = 12;
  b.hitch_every_s = 6;
  a.hitch_burst = b.hitch_burst = 4;
  {
    RelayMatch::Side quiet_a = a, quiet_b = b;
    quiet_a.report_hitches = quiet_b.report_hitches = false;
    RelayMatch unreported(Config{}, quiet_a, quiet_b);
    unreported.Play(12000);
    unreported.Print("LAN, hitches unreported");
    unreported.ExpectClean();
    EXPECT_GE(unreported.SessionOf(0).Delay(), 4);
  }
  RelayMatch match(Config{}, a, b);
  match.Play(12000);
  match.Print("LAN, hitches");
  match.ExpectClean();
  const int hitches = match.HitchesOf(0) + match.HitchesOf(1);
  EXPECT_GT(match.HitchesOf(1), 25);
  for (int i = 0; i < 2; ++i)
  {
    const Stats& stats = match.SessionOf(i).GetStats();
    EXPECT_LE(match.SessionOf(i).Delay(), 3) << "side " << i;
    EXPECT_EQ(stats.delay_raises, 0) << "side " << i;
    // The stalls are the hitches': none beyond them.
    EXPECT_LE(stats.stalls, hitches + 1) << "side " << i;
  }
  EXPECT_GT(match.CompareConfirmed(), 11000);
}

// Fuzz: random links, clock drift and wildly swinging round-trip reports (each side's delay jumps
// up and down, by several frames at once in the first seconds), with time sync on. Whatever the
// delays do, both machines must end up having run every confirmed frame with identical inputs,
// and with identical states.
TEST(OrcaSessionFuzz, BothSidesRunIdenticalInputsUnderDelaySwings)
{
  for (u32 seed = 1; seed <= 60; ++seed)
  {
    std::mt19937 rng(seed);
    const auto pick = [&rng](int lo, int hi) {
      return std::uniform_int_distribution<int>(lo, hi)(rng);
    };
    Config config;
    config.input_delay = pick(0, 3);
    config.max_delay = config.input_delay + pick(0, 5);
    Match match(config, pick(0, 8), pick(0, 6));
    match.net.extra = {pick(0, 4), pick(0, 4)};
    match.a_extra_every = pick(0, 1) ? pick(20, 200) : 0;
    match.a->tag_ticks = true;
    match.b->tag_ticks = true;
    for (int chunk = 0; chunk < 60; ++chunk)
    {
      // Round trips anywhere from LAN to intercontinental, at random moments, per side.
      for (Player* p : {match.a.get(), match.b.get()})
      {
        if (pick(0, 3) == 0)
          p->session.OnRoundTrip(pick(5, 450));
      }
      match.Play(pick(5, 60));
    }
    match.Play(400);  // settle

    SCOPED_TRACE(::testing::Message() << "seed " << seed);
    ExpectClean(match);
    const int confirmed = std::min(Settled(*match.a), Settled(*match.b));
    ASSERT_GT(confirmed, 500);
    int checked = 0;
    for (int f = 0; f <= confirmed; ++f)
    {
      const auto ra = match.a->game.m_ran_pads.find(f);
      const auto rb = match.b->game.m_ran_pads.find(f);
      if (ra == match.a->game.m_ran_pads.end() || rb == match.b->game.m_ran_pads.end())
        continue;
      ASSERT_EQ(ra->second, rb->second) << "frame " << f;
      ++checked;
    }
    EXPECT_GT(checked, 300);
    EXPECT_GT(match.CompareConfirmed(), 300);
  }
}

