// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/OnlineMatch.h"

#include <chrono>
#include <memory>
#include <string>

#include <fmt/format.h>

#include "Common/HookableEvent.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Rollback/SessionPort.h"
#include "Core/System.h"

namespace Rollback::OnlineMatch
{
namespace
{
struct Match
{
  bool started = false;
  bool finished = false;
  std::unique_ptr<RingPort> port;
  std::unique_ptr<Orca::Net::Session> session;
  int local_seat = 0;
  // The frame the current boundary completes (-1 at the first boundary).
  int running = -1;
  std::chrono::steady_clock::time_point last_round_trip{};
  Common::EventHook on_state_changed;
};

Match& TheMatch()
{
  static Match match;
  return match;
}

bool Stopping(Core::System& system)
{
  return Core::GetState(system) == Core::State::Stopping;
}

// The room's round trip feeds the session's adaptive input delay, once the session takes it.
template <typename SessionType>
void ReportRoundTrip(SessionType& session, int round_trip_ms)
{
  if constexpr (requires { session.OnRoundTrip(round_trip_ms); })
    session.OnRoundTrip(round_trip_ms);
}
}  // namespace

std::optional<int> OnBoundary(Core::System& system,
                              const std::function<Orca::Net::Pad(int local_seat)>& local_pad)
{
  Match& match = TheMatch();
  if (match.finished)
    return std::nullopt;

  if (!match.started)
  {
    match.started = true;
    // Once emulation has stopped (the CPU thread is gone), leave the room.
    match.on_state_changed = Core::AddOnStateChangedCallback([](Core::State state) {
      if (state == Core::State::Uninitialized)
        End("emulation stopped");
    });

    Orca::Net::Config config;
    Orca::Net::Transport* transport =
        Orca::Online::WaitForMatch(&config, [&system] { return Stopping(system); });
    if (!transport)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Online: no match ({})", Orca::Online::StatusLine());
      End("no match");
      return std::nullopt;
    }
    match.local_seat = config.local_seat;
    match.port = std::make_unique<RingPort>(system, config.max_rollback);
    match.session = std::make_unique<Orca::Net::Session>(config, *match.port, *transport);
    NOTICE_LOG_FMT(ROLLBACK,
                   "Online: match started: seat {} of {}, input delay {}, max rollback {}, seed "
                   "{:08x}, round trip {} ms",
                   config.local_seat + 1, config.seats, config.input_delay, config.max_rollback,
                   Orca::Online::Seed(), Orca::Online::RoundTripMs());
  }
  if (!match.session)
    return std::nullopt;

  if (const auto now = std::chrono::steady_clock::now();
      now - match.last_round_trip >= std::chrono::seconds{1})
  {
    match.last_round_trip = now;
    if (const int round_trip_ms = Orca::Online::RoundTripMs(); round_trip_ms >= 0)
      ReportRoundTrip(*match.session, round_trip_ms);
  }

  const Orca::Net::Step step =
      StepSession(*match.session, match.running, local_pad(match.local_seat),
                  [&system] { return Stopping(system); }, std::chrono::milliseconds{1});
  switch (step.kind)
  {
  case Orca::Net::StepKind::Run:
    match.running = step.frame;
    return std::nullopt;
  case Orca::Net::StepKind::Rollback:
    match.running = step.frame;
    return step.frame;
  default:
  {
    const std::string error = match.session->Error();
    Core::DisplayMessage(fmt::format("Online match ended: {}", error.empty() ? "stopped" : error),
                         10000);
    End("session ended");
    if (!Stopping(system))
      Core::QueueHostJob([](Core::System& host_system) { Core::Stop(host_system); });
    return std::nullopt;
  }
  }
}

void LogStats(const char* when)
{
  const Match& match = TheMatch();
  if (!match.session)
    return;
  const Orca::Net::Stats& stats = match.session->GetStats();
  NOTICE_LOG_FMT(ROLLBACK,
                 "Online match {}: frame {}, {} rollbacks, {} re-run frames, deepest {}, {} stalls, "
                 "{} checksums matched, confirmed frame {}, round trip {} ms{}{}",
                 when, match.session->CurrentFrame(), stats.rollbacks, stats.resimulated_frames,
                 stats.deepest_rollback, stats.stalls, stats.checksums_matched,
                 stats.confirmed_frame, Orca::Online::RoundTripMs(),
                 match.session->Error().empty() ? "" : "; ", match.session->Error());
}

bool Active()
{
  return TheMatch().session != nullptr;
}

void End(const char* reason)
{
  Match& match = TheMatch();
  if (!match.started || match.finished)
    return;
  match.finished = true;
  LogStats(fmt::format("ended ({})", reason).c_str());
  match.session.reset();
  match.port.reset();
  Orca::Online::Shutdown();
}
}  // namespace Rollback::OnlineMatch
