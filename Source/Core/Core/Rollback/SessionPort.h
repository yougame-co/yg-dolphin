// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The emulator side of an Orca rollback session: Orca::Net::Game over the snapshot ring.

#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <utility>

#include "Common/CommonTypes.h"
#include "Core/CoreTiming.h"
#include "Core/Orca/Session/Session.h"
#include "Core/Rollback/Rollback.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class System;
}

namespace Rollback
{
// Create and destroy on the CPU thread at a frame boundary, or after emulation has stopped: the
// destructor resets the ring and the NAND journal, and SI polls read the active port's pads.
class RingPort final : public Orca::Net::Game
{
public:
  // `max_rollback` is the session's Config::max_rollback. `unthrottled` (tests) never throttles.
  RingPort(Core::System& system, int max_rollback, bool unthrottled = false);
  ~RingPort() override;
  RingPort(const RingPort&) = delete;
  RingPort& operator=(const RingPort&) = delete;

  bool Save(int frame) override;
  bool Load(int frame) override;
  void SetPads(int frame, const Orca::Net::Pads& pads) override;
  void SetResimulating(bool resimulating) override;
  std::optional<u64> SnapshotChecksum(int frame) override;
  void ResetPacing() override;

  // What GameCube port `port` (0-3) reports while the current frame runs: decoded from the
  // session's pads, the local player's included, so both machines see the same bytes.
  GCPadStatus Pad(int port) const { return m_pads[port]; }

  // The port of the running session, or null.
  static RingPort* Active();

private:
  Core::System& m_system;
  // The newest max_rollback + 2 saves. The session saves only unconfirmed frames and checksum
  // frames, and stalls once the newest frame is max_rollback past the confirmed one, so a rollback
  // target or a checksum frame is asked for before more than max_rollback newer frames are saved.
  SnapshotRing m_ring;
  std::array<GCPadStatus, Orca::Net::MAX_SEATS> m_pads;
  bool m_unthrottled;
  bool m_resimulating = false;
  // The frame saved since the emulator last ran: a stalled boundary asks again for the same one.
  int m_saved_since_run = -1;
  // The throttle's reference before the rollback load, put back when the re-run catches up.
  std::optional<std::pair<s64, TimePoint>> m_reference_before_load;
};

// One frame boundary of a session: reports the frame that completed and the local pad, then does
// what the session says. While it stalls, waits `stall_wait` and asks again; when it asks this
// machine to wait (time sync), holds one frame period and asks again; until it moves on or
// `stopping()` is true. Returns the last step: Run or Rollback (the next frame has its pads, and
// after a Rollback the state is the start of step.frame), or Ended (Session::Error says why;
// stopping also ends with Ended). A stall or wait that lasts longer than `give_up_after` of wall
// time (zero: never) also ends with Ended and sets *gave_up: the other side stopped answering.
Orca::Net::Step StepSession(Orca::Net::Session& session, RingPort& port, int completed_frame,
                            const Orca::Net::Pad& local_pad, const std::function<bool()>& stopping,
                            std::chrono::microseconds stall_wait,
                            std::chrono::milliseconds give_up_after = {}, bool* gave_up = nullptr);
}  // namespace Rollback
