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
  u32 round_trip_sequence = 0;
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

// A round-trip sample the session has not seen yet (the room numbers its samples).
std::optional<int> NewRoundTrip(u32* last_sequence)
{
  u32 sequence = 0;
  const int round_trip_ms = Orca::Online::RoundTripMs(&sequence);
  if (round_trip_ms < 0 || sequence == *last_sequence)
    return std::nullopt;
  *last_sequence = sequence;
  return round_trip_ms;
}

void StopEmulation(Core::System& system, const std::string& message)
{
  Core::DisplayMessage(message, 10000);
  if (!Stopping(system))
    Core::QueueHostJob([](Core::System& host_system) { Core::Stop(host_system); });
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
      // Every controller reports neutral in a session: don't leave the game running without one.
      const std::string status = Orca::Online::StatusLine();
      NOTICE_LOG_FMT(ROLLBACK, "Online: no match ({})", status);
      End("no match");
      StopEmulation(system, fmt::format("No online match: {}", status));
      return std::nullopt;
    }
    match.local_seat = config.local_seat;
    match.port = std::make_unique<RingPort>(system, config.max_rollback);
    match.session = std::make_unique<Orca::Net::Session>(config, *match.port, *transport);
    // The wait for the match is not time the game should make up.
    match.port->ResetPacing();
    NOTICE_LOG_FMT(ROLLBACK,
                   "Online: match started: seat {} of {}, input delay {}, max rollback {}, seed "
                   "{:08x}, round trip {} ms",
                   config.local_seat + 1, config.seats, config.input_delay, config.max_rollback,
                   Orca::Online::Seed(), Orca::Online::RoundTripMs());
  }
  if (!match.session)
    return std::nullopt;

  // Round trips feed the session's adaptive input delay.
  if (const auto round_trip_ms = NewRoundTrip(&match.round_trip_sequence))
    match.session->OnRoundTrip(*round_trip_ms);

  const Orca::Net::Step step =
      StepSession(*match.session, *match.port, match.running, local_pad(match.local_seat),
                  [&system] { return Stopping(system); }, std::chrono::milliseconds{1});
  if (step.kind == Orca::Net::StepKind::Run || step.kind == Orca::Net::StepKind::Rollback)
  {
    match.running = step.frame;
    return step.kind == Orca::Net::StepKind::Rollback ? std::optional<int>(step.frame) :
                                                        std::nullopt;
  }
  if (step.kind != Orca::Net::StepKind::Ended)
  {
    // StepSession handles every other step itself; reaching this is a bug, not the end of a match.
    ERROR_LOG_FMT(ROLLBACK, "Online: unexpected session step {} at frame {}",
                  static_cast<int>(step.kind), step.frame);
    return std::nullopt;
  }
  const std::string error = match.session->Error();
  End("session ended");
  StopEmulation(system, fmt::format("Online match ended: {}", error.empty() ? "stopped" : error));
  return std::nullopt;
}

void LogStats(const char* when)
{
  const Match& match = TheMatch();
  if (!match.session)
    return;
  const Orca::Net::Stats& stats = match.session->GetStats();
  NOTICE_LOG_FMT(ROLLBACK,
                 "Online match {}: frame {}, {} rollbacks, {} re-run frames, deepest {}, {} stalls, "
                 "{} waits, frame advantage {:.2f}, input delay {} (+{} -{} reverted {}), {} "
                 "checksums matched, confirmed frame {}, round trip {} ms{}{}",
                 when, match.session->CurrentFrame(), stats.rollbacks, stats.resimulated_frames,
                 stats.deepest_rollback, stats.stalls, stats.waits, stats.frame_advantage,
                 stats.delay, stats.delay_raises, stats.delay_lowers, stats.delay_reverts,
                 stats.checksums_matched, stats.confirmed_frame, Orca::Online::RoundTripMs(),
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
