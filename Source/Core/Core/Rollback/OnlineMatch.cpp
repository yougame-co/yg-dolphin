// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/OnlineMatch.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <fmt/format.h>

#include "Common/HookableEvent.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
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
  // Emulation stopped since this match started: the next boot plays a new match.
  bool stale = false;
  std::unique_ptr<RingPort> port;
  std::unique_ptr<Orca::Net::Session> session;
  int local_seat = 0;
  // The frame the current boundary completes (-1 at the first boundary).
  int running = -1;
  u32 round_trip_sequence = 0;
  // When the last boundary handed control back to the emulator: the next boundary times the frame
  // from there, leaving out the session's own stalls and waits.
  std::optional<std::chrono::steady_clock::time_point> last_return;
  Common::EventHook on_state_changed;
};

Match& TheMatch()
{
  static Match match;
  return match;
}

// Emulation is stopping, or the host asked the CPU thread to pause (a pause, a stop with
// confirmation, a PauseAndLock): its caller waits for the CPU thread to go idle, so a loop that
// holds the CPU thread here must give up rather than deadlock it.
bool Stopping(Core::System& system)
{
  return Core::GetState(system) == Core::State::Stopping ||
         system.GetCPU().GetState() != CPU::State::Running;
}

// Two frame periods at 60 Hz: a frame slower than this was a hitch.
constexpr std::chrono::microseconds HITCH_TIME{33'334};

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
  if (match.stale)
  {
    // A new boot in the same process (the room was left when the last one stopped).
    match.started = false;
    match.finished = false;
    match.stale = false;
    match.local_seat = 0;
    match.running = -1;
    match.round_trip_sequence = 0;
    match.last_return.reset();
  }
  if (match.finished)
    return std::nullopt;

  if (!match.started)
  {
    match.started = true;
    // Once emulation has stopped (the CPU thread is gone), leave the room.
    if (!match.on_state_changed)
    {
      match.on_state_changed = Core::AddOnStateChangedCallback([](Core::State state) {
        if (state != Core::State::Uninitialized)
          return;
        End("emulation stopped");
        TheMatch().stale = true;
      });
    }

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
  // A frame that took more than two frame periods of wall time was a hitch on this machine (a disc
  // read, a shader compile): the other side stalls on it, and no input delay would have helped.
  const auto now = std::chrono::steady_clock::now();
  if (match.last_return && now - *match.last_return > HITCH_TIME)
    match.session->OnLocalHitch();

  const Orca::Net::Step step =
      StepSession(*match.session, *match.port, match.running, local_pad(match.local_seat),
                  [&system] { return Stopping(system); }, std::chrono::milliseconds{1});
  match.last_return = std::chrono::steady_clock::now();
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
  NOTICE_LOG_FMT(
      ROLLBACK,
      "Online match {}: frame {}, {} rollbacks, {} re-run frames, deepest {}, {} stalls ({} "
      "counted), {} waits, {} hitches here, {} there, frame advantage {:.2f}, input delay {} "
      "(+{} -{} reverted {}), {} checksums matched, confirmed frame {}, round trip {} ms{}{}",
      when, match.session->CurrentFrame(), stats.rollbacks, stats.resimulated_frames,
      stats.deepest_rollback, stats.stalls, stats.counted_stalls, stats.waits, stats.hitches,
      stats.peer_hitches, stats.frame_advantage,
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
