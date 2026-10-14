// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

namespace Orca::Net
{
using RoomClock = std::chrono::steady_clock;
// Transport policy only: neither timer participates in the simulated game.
inline constexpr auto ROOM_HOLD = std::chrono::seconds(90);
inline constexpr auto PEER_SILENCE_LIMIT = std::chrono::seconds(10);
inline constexpr auto DIRECT_INPUT_WINDOW = std::chrono::milliseconds(500);

inline bool DirectInputLive(std::optional<RoomClock::time_point> input, RoomClock::time_point now)
{
  return input && now - *input < DIRECT_INPUT_WINDOW;
}

enum class RoomLoss
{
  None,
  InputSilent,
  HoldExpired,
};

// A held room seat is still the same player. Once its session is suspended, the next attach
// needs a fresh hello/keyframe, but its hold deadline remains available to the waiting joiner.
struct PeerRoomPresence
{
  std::optional<RoomClock::time_point> away_since;
  bool suspended = false;

  void Away(RoomClock::time_point now)
  {
    if (!away_since)
      away_since = now;
  }

  std::optional<RoomClock::time_point> Deadline() const
  {
    return away_since ? std::optional(*away_since + ROOM_HOLD) : std::nullopt;
  }

  RoomLoss Check(RoomClock::time_point now,
                 std::optional<RoomClock::time_point> last_direct_input) const
  {
    if (!away_since)
      return RoomLoss::None;
    if (now >= *Deadline())
      return RoomLoss::HoldExpired;
    if (!suspended && now - last_direct_input.value_or(*away_since) >= PEER_SILENCE_LIMIT)
      return RoomLoss::InputSilent;
    return RoomLoss::None;
  }

  // Also handles a same-id join when the server missed the old socket's close.
  bool Back(RoomClock::time_point now, bool direct_live)
  {
    const bool keep = !suspended && (away_since ? now < *Deadline() : direct_live);
    away_since.reset();
    suspended = false;
    return keep;
  }
};

struct RoomReconnect
{
  std::optional<RoomClock::time_point> since;
  std::optional<RoomClock::time_point> next_attempt;
  unsigned attempts = 0;
  std::chrono::milliseconds backoff{500};

  bool Start(RoomClock::time_point now, unsigned close_code)
  {
    if (close_code == 4000 || close_code == 4004)
      return false;
    if (!since)
    {
      since = now;
      next_attempt = now + std::chrono::milliseconds(500);
      attempts = 0;
      backoff = std::chrono::milliseconds(500);
    }
    return true;
  }

  RoomLoss Check(RoomClock::time_point now) const
  {
    if (since && now >= *since + ROOM_HOLD)
      return RoomLoss::HoldExpired;
    return RoomLoss::None;
  }

  bool Attempt(RoomClock::time_point now)
  {
    if (!since || !next_attempt || now < *next_attempt || Check(now) != RoomLoss::None)
      return false;
    next_attempt.reset();
    ++attempts;
    return true;
  }

  void Retry(RoomClock::time_point now)
  {
    if (!since)
      return;
    next_attempt = std::min(now + backoff, *since + ROOM_HOLD);
    backoff = std::min(backoff * 2, std::chrono::milliseconds(4000));
  }
};

// After the host's session goes away, the joiner stays in the room while its seat is held. Back
// starts the ordinary drop-in path; a real leave or an expired hold abandons the wait.
struct HostRoomWait
{
  std::optional<RoomClock::time_point> until;

  bool Start(RoomClock::time_point now, std::optional<RoomClock::time_point> deadline)
  {
    if (!deadline || now >= *deadline)
      return false;
    until = deadline;
    return true;
  }

  bool Expired(RoomClock::time_point now) const { return until && now >= *until; }
  void Back() { until.reset(); }
};
}  // namespace Orca::Net
