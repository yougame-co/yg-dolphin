// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <deque>
#include <utility>
#include <map>
#include <optional>
#include <random>
#include <vector>

#include <cstdlib>
#include <string>

#include <gtest/gtest.h>
#include <picojson.h>

#include "Common/HttpRequest.h"
#include "Common/WebSocket.h"

#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Session/Session.h"

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
    m_snapshots[frame] = m_state;
    return true;
  }
  bool Load(int frame) override
  {
    const auto it = m_snapshots.find(frame);
    if (it == m_snapshots.end())
      return false;
    m_state = it->second;
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
  std::mt19937 rng{1234};

  class End final : public Transport
  {
  public:
    End(FakeNet& net, int index) : m_net(net), m_index(index) {}
    void Send(const Packet& packet) override
    {
      int delay = m_net.latency;
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
  void Expect(const Pad& pad, int input_delay)
  {
    if (expected.empty())
    {
      for (int f = 0; f < input_delay; ++f)
        expected[f] = Pad{};
    }
    const int target = session.CurrentFrame() + session.Delay();
    const int newest = expected.empty() ? -1 : expected.rbegin()->first;
    if (target <= newest)
    {
      if (session.Delay() < last_delay)
        ++dropped_after_lower;
      last_delay = session.Delay();
      return;
    }
    if (target > newest + 1 && newest >= 0)
      ++filled;
    const Pad held = newest >= 0 ? expected.rbegin()->second : Pad{};
    for (int f = newest + 1; f < target; ++f)
      expected[f] = held;
    expected[target] = pad;
    last_delay = session.Delay();
  }

  // One tick of wall time: re-run any frames a rollback needs, then run at most one new frame.
  void Tick(int tick)
  {
    if (model_input_delay >= 0 && !session.Resimulating())
      Expect(LocalPad(tick), model_input_delay);
    for (int guard = 0; guard < 64; ++guard)
    {
      const Step step = session.OnBoundary(completed, LocalPad(tick));
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
      // Both ends measure the round trip every half second, as the relay's pings would.
      if (rtt_samples && t % 30 == 0)
      {
        for (Player* p : {a.get(), b.get()})
        {
          int rtt = 2 * net.latency;
          if (net.jitter > 0)
          {
            rtt += std::uniform_int_distribution<int>(0, net.jitter)(rtt_rng) +
                   std::uniform_int_distribution<int>(0, net.jitter)(rtt_rng);
          }
          p->session.OnRoundTrip(rtt * 1000 / 60);
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
  std::mt19937 rtt_rng{99};
  int a_extra_every = 0;
  int played = 0;
};

void ExpectClean(const Match& match)
{
  for (const Player* p : {match.a.get(), match.b.get()})
    EXPECT_TRUE(p->session.Error().empty()) << p->session.Error();
}
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
  Match match(Config{}, 3, 0);
  match.rtt_samples = true;
  // 100 ms round trip: 3 frames one way at 60 Hz, steady.
  match.Play(1);
  EXPECT_EQ(match.a->session.Delay(), 3);
  EXPECT_EQ(match.b->session.Delay(), 3);
  // Four quiet seconds (a lower needs five): no rollback at all.
  match.Play(239);
  ExpectClean(match);
  EXPECT_EQ(match.a->session.GetStats().rollbacks, 0);
  EXPECT_EQ(match.b->session.GetStats().rollbacks, 0);
  EXPECT_GT(match.CompareConfirmed(), 200);

  // A huge round trip is capped at max_delay; after the first seconds, round trips move nothing.
  Match far(Config{}, 0, 0);
  far.a->session.OnRoundTrip(1000);
  EXPECT_EQ(far.a->session.Delay(), Config{}.max_delay);
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
  // A reports a 200 ms round trip before the match starts, so it opens at max_delay; the link is
  // really instant, so quiet windows bring it back down one frame at a time. B stays at 2 all
  // along: the two delays differ for the whole match and the frames still agree.
  Match match(Config{}, 0, 0);
  for (Player* p : {match.a.get(), match.b.get()})
  {
    p->tag_ticks = true;
    p->model_input_delay = Config{}.input_delay;
  }
  match.a->session.OnRoundTrip(200);
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
  // Four frames of latency, measured: both open at 4, where inputs arrive just in time. A step
  // down to 3 makes the other side roll back, so it reverts, and the next try waits longer.
  Match match(Config{}, 4, 0);
  match.rtt_samples = true;
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
  const auto play = [&config](bool time_sync, int extra_every) {
    config.time_sync = time_sync;
    auto match = std::make_unique<Match>(config, 3, 1);
    match->rtt_samples = true;
    match->a_extra_every = extra_every;
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
  // deep all the way. With it, it never stalls and holds about the 1.5-frame threshold ahead.
  EXPECT_GT(unsynced->a->session.GetStats().stalls, 30);
  EXPECT_EQ(a.stalls, 0);
  EXPECT_LT(a.resimulated_frames * 4, unsynced->a->session.GetStats().resimulated_frames * 3);
  EXPECT_LT(a.frame_advantage, 3.0);

  // Equal clocks: nobody waits.
  const auto even = play(true, 0);
  EXPECT_EQ(even->a->session.GetStats().waits, 0);
  EXPECT_EQ(even->b->session.GetStats().waits, 0);
}
