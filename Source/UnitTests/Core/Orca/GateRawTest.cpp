// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <climits>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <picojson.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/Session.h"
#include "SessionTestHarness.h"

namespace Orca::Net::Test
{
namespace
{
class GateRawConfig final : public ::Config::ConfigLayerLoader
{
public:
  GateRawConfig() : ConfigLayerLoader(::Config::LayerType::Base) {}
  void Load(::Config::Layer*) override {}
  void Save(::Config::Layer*) override {}
};

class GateRawListener final : public Common::Log::LogListener
{
public:
  void Log(Common::Log::LogLevel, const char* message) override { lines += message; }
  std::string lines;
};

class OrcaGateRaw : public testing::Test
{
protected:
  void SetUp() override
  {
    // Keep the logger's config and file sink inside the test working directory.
    old_user = File::GetUserPath(D_USER_IDX);
    directory = File::GetCurrentDir() + "/gate-raw-test-user/";
    ASSERT_TRUE(File::CreateFullPath(directory));
    File::SetUserPath(D_USER_IDX, directory);
    ::Config::Init();
    ::Config::AddLayer(std::make_unique<GateRawConfig>());
    Common::Log::LogManager::Init();
    auto listener = std::make_unique<GateRawListener>();
    capture = listener.get();
    auto* logger = Common::Log::LogManager::GetInstance();
    logger->RegisterListener(Common::Log::LogListener::LOG_WINDOW_LISTENER, std::move(listener));
    logger->EnableListener(Common::Log::LogListener::LOG_WINDOW_LISTENER, true);
    logger->EnableListener(Common::Log::LogListener::FILE_LISTENER, false);
    logger->EnableListener(Common::Log::LogListener::CONSOLE_LISTENER, false);
    logger->SetEnable(Common::Log::LogType::ROLLBACK, true);
  }
  void TearDown() override
  {
    Common::Log::LogManager::Shutdown();
    ::Config::Shutdown();
    File::SetUserPath(D_USER_IDX, old_user);
    File::DeleteDirRecursively(directory);
  }
  std::vector<picojson::value> Events() const
  {
    std::vector<picojson::value> events;
    size_t next = 0;
    while ((next = capture->lines.find("ORCA_GATE_RAW ", next)) != std::string::npos)
    {
      next += std::string("ORCA_GATE_RAW ").size();
      const auto end = capture->lines.find('\n', next);
      picojson::value value;
      EXPECT_TRUE(picojson::parse(value, capture->lines.substr(next, end - next)).empty());
      events.push_back(std::move(value));
      next = end;
    }
    return events;
  }
  void Summary(const Session& session, const char* when)
  {
    Orca::Gate::EmitRaw(session, Orca::Gate::Event::Summary);
    const auto& s = session.GetStats();
    NOTICE_LOG_FMT(ROLLBACK,
                   "Online match at {}: frame {}, {} rollbacks, {} re-run frames, deepest {}, "
                   "{} stalls ({} counted), {} waits, {} hitches here ({} more in game loads)",
                   when, session.CurrentFrame(), s.rollbacks, s.resimulated_frames,
                   s.deepest_rollback, s.stalls, s.counted_stalls, s.waits, s.hitches, s.load_hitches);
  }
  GateRawListener* capture = nullptr;
  std::string old_user;
  std::string directory;
};

struct GateRawTransport final : Transport
{
  void Send(const Packet& packet) override { packets.push_back(EncodePacket(packet)); }
  std::vector<Packet> Receive() override
  {
    if (!allow)
      return {};
    Packet packet;
    packet.seat = 1;
    packet.first_frame = next;
    const int frame = session->CurrentFrame();
    for (; next <= frame + 2; ++next)
      packet.pads.push_back(incoming_pad);
    packet.current_frame = frame;
    packet.ack = {frame, -1, -1, -1};
    return {packet};
  }
  bool Connected() const override { return true; }
  Session* session = nullptr;
  bool allow = true;
  Pad incoming_pad{};
  int next = 0;
  std::vector<std::string> packets;
};
}  // namespace

TEST_F(OrcaGateRaw, BeginAndHitchFormatting)
{
  FakeGame game;
  GateRawTransport transport;
  Session session(Config{}, game, transport);
  using namespace std::chrono_literals;
  Orca::Gate::EmitRaw(session, Orca::Gate::Event::HitchTiming, 40ms, 0ms);
  session.OnLocalHitch();
  Orca::Gate::EmitRaw(session, Orca::Gate::Event::HitchTiming, 50ms, 20ms);
  session.OnLoadHitch();
  const auto events = Events();
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[0].serialize(), R"({"kind":"begin","v":1})");
  picojson::value expected;
  ASSERT_TRUE(picojson::parse(expected, R"({"v":1,"kind":"hitch","id":1,"frame":0,"ms":40,"held_ms":0,"class":"here"})").empty());
  EXPECT_EQ(events[1], expected);
  ASSERT_TRUE(picojson::parse(expected, R"({"v":1,"kind":"hitch","id":2,"frame":0,"ms":50,"held_ms":20,"class":"load"})").empty());
  EXPECT_EQ(events[2], expected);
  Summary(session, "leave");
  fmt::print("GATE_RAW_CAPTURE_BEGIN\n{}GATE_RAW_CAPTURE_END\n", capture->lines);
}

TEST_F(OrcaGateRaw, ZeroEventsStillBeginsBeforeSummary)
{
  FakeGame game;
  GateRawTransport transport;
  {
    Session session(Config{}, game, transport);
  }
  ASSERT_EQ(Events().size(), 1u);
  fmt::print("GATE_RAW_CAPTURE_BEGIN\n{}GATE_RAW_CAPTURE_END\n", capture->lines);
}

TEST_F(OrcaGateRaw, RollbackDoesNotExtendStallWait)
{
  for (const bool finish : {true, false})
  {
    capture->lines.clear();
    FakeGame game;
    GateRawTransport transport;
    Config config;
    config.time_sync = false;
    config.fixed_delay = 2;
    int frame = -1;
    {
      Session session(config, game, transport);
      transport.session = &session;
      int completed = -1;
      const auto boundary = [&] {
        const auto step = session.OnBoundary(completed, Pad{});
        EXPECT_NE(step.kind, StepKind::Ended) << session.Error();
        if (step.kind == StepKind::Run || step.kind == StepKind::Rollback)
          completed = game.Run();
        return step;
      };
      for (int i = 0; i < 700; ++i)
        ASSERT_EQ(boundary().kind, StepKind::Run);
      transport.allow = false;
      bool stalled = false;
      for (int i = 0; i < 20 && !stalled; ++i)
        stalled = boundary().kind == StepKind::Stall;
      ASSERT_TRUE(stalled);
      frame = session.CurrentFrame();
      game.now_ms = 20.5;
      transport.allow = true;
      transport.incoming_pad[0] = 1;
      ASSERT_EQ(boundary().kind, StepKind::Rollback);
      game.now_ms = 200;
      if (finish)
      {
        for (int i = 0; i < config.max_rollback + 2 && session.CurrentFrame() == frame; ++i)
          boundary();
        ASSERT_GT(session.CurrentFrame(), frame);
        Summary(session, "leave");
        // Repeated summaries must not duplicate the finalized stall.
        Summary(session, "leave");
      }
      EXPECT_EQ(session.GetStats().stalls, 1);
      EXPECT_EQ(session.GetStats().counted_stalls, finish ? 1 : 0);
    }
    const auto events = Events();
    ASSERT_EQ(events.size(), 2u);
    picojson::value expected;
    ASSERT_TRUE(picojson::parse(expected, fmt::format(
        R"({{"v":1,"kind":"stall","id":1,"frame":{},"wait_ms":20.5,"frames_ahead":8,"after_input":{},"plugged":true,"settle_frame":600,"last_hitch_frame":{},"last_peer_stall_frame":{}}})",
        frame, finish, INT_MIN / 2, INT_MIN / 2)).empty());
    EXPECT_EQ(events[1], expected);
    fmt::print("GATE_RAW_CAPTURE_BEGIN\n{}GATE_RAW_CAPTURE_END\n", capture->lines);
  }
}

TEST_F(OrcaGateRaw, ScriptedWindowsAndDisabledSinkPreserveBehaviour)
{
  struct Result
  {
    Stats stats;
    std::vector<std::pair<StepKind, int>> steps;
    std::vector<std::string> packets;
    u64 state;
    int resets;
  };
  const auto run = [&](bool sink) {
    Common::Log::LogManager::GetInstance()->EnableListener(
        Common::Log::LogListener::LOG_WINDOW_LISTENER, sink);
    capture->lines.clear();
    FakeGame game;
    GateRawTransport transport;
    Config config;
    config.time_sync = false;
    config.fixed_delay = 2;
    Session session(config, game, transport);
    transport.session = &session;
    Result result;
    int completed = -1;
    const auto boundary = [&] {
      const auto step = session.OnBoundary(completed, Pad{});
      result.steps.emplace_back(step.kind, step.frame);
      EXPECT_NE(step.kind, StepKind::Ended) << session.Error();
      if (step.kind == StepKind::Run || step.kind == StepKind::Rollback)
        completed = game.Run();
      return step.kind;
    };
    const auto play = [&](int frame) {
      while (session.CurrentFrame() < frame)
      {
        if (boundary() == StepKind::Stall || !session.Error().empty())
        {
          ADD_FAILURE() << "Unexpected stall";
          break;
        }
      }
    };
    const auto start_stall = [&] {
      transport.allow = false;
      for (int attempts = 0; attempts < 20; ++attempts)
        if (boundary() == StepKind::Stall)
          return;
      ADD_FAILURE() << "No stall";
    };
    play(700);
    start_stall();
    game.now_ms += 20.5;
    transport.allow = true;
    EXPECT_EQ(boundary(), StepKind::Run);
    play(1000);
    using namespace std::chrono_literals;
    Orca::Gate::EmitRaw(session, Orca::Gate::Event::HitchTiming, 40ms, 0ms);
    session.OnLocalHitch();
    start_stall();
    game.now_ms += 35;
    transport.allow = true;
    EXPECT_EQ(boundary(), StepKind::Run);
    play(1800);
    Summary(session, "frame 1800");
    play(3600);
    Summary(session, "frame 3600");
    start_stall();
    game.now_ms += 12;
    // Session destruction must flush this unfinished stall and close the tail.
    result.stats = session.GetStats();
    result.packets = transport.packets;
    result.state = game.m_state;
    result.resets = game.m_pacing_resets;
    return result;
  };
  const auto enabled = run(true);
  const auto events = Events();
  ASSERT_EQ(events.size(), 5u);
  for (size_t index = 1; index < events.size(); ++index)
    EXPECT_EQ(events[index].get("id").get<double>(), index);
  EXPECT_EQ(events[1].get("kind").get<std::string>(), "stall");
  EXPECT_DOUBLE_EQ(events[1].get("wait_ms").get<double>(), 20.5);
  EXPECT_EQ(events[1].get<picojson::object>().size(), 11u);
  EXPECT_TRUE(events[1].get("after_input").get<bool>());
  EXPECT_TRUE(events[1].get("plugged").get<bool>());
  EXPECT_EQ(events[1].get("settle_frame").get<double>(), 600);
  EXPECT_EQ(events[1].get("frames_ahead").get<double>(), 8);
  EXPECT_LT(events[1].get("last_hitch_frame").get<double>(),
            events[1].get("frame").get<double>() - 30);
  EXPECT_GE(events[3].get("last_hitch_frame").get<double>(),
            events[3].get("frame").get<double>() - 30);
  EXPECT_EQ(events[3].get("kind").get<std::string>(), "stall");
  EXPECT_DOUBLE_EQ(events[3].get("wait_ms").get<double>(), 35);
  EXPECT_EQ(events[4].get("kind").get<std::string>(), "stall");
  EXPECT_DOUBLE_EQ(events[4].get("wait_ms").get<double>(), 12);
  EXPECT_FALSE(events[4].get("after_input").get<bool>());
  EXPECT_EQ(enabled.stats.stalls, 3);
  EXPECT_EQ(enabled.stats.counted_stalls, 1);
  EXPECT_EQ(enabled.stats.hitches, 1);
  fmt::print("GATE_RAW_CAPTURE_BEGIN\n{}GATE_RAW_CAPTURE_END\n", capture->lines);
  const auto disabled = run(false);
  EXPECT_EQ(enabled.steps, disabled.steps);
  EXPECT_EQ(enabled.packets, disabled.packets);
  EXPECT_EQ(enabled.state, disabled.state);
  EXPECT_EQ(enabled.resets, disabled.resets);
  const auto stats = [](const Stats& s) {
    return std::tie(s.rollbacks, s.resimulated_frames, s.deepest_rollback, s.saves,
                    s.snapshot_every, s.stalls, s.checksums_matched, s.confirmed_frame,
                    s.delay, s.delay_fixed, s.delay_raises, s.delay_lowers, s.delay_reverts,
                    s.waits, s.frame_advantage, s.hitches, s.peer_hitches, s.load_hitches,
                    s.counted_stalls, s.spared_stalls, s.rtt_median, s.peer_rtt_median,
                    s.late_seats);
  };
  EXPECT_EQ(stats(enabled.stats), stats(disabled.stats));
  EXPECT_TRUE(capture->lines.empty());
}
}  // namespace Orca::Net::Test
