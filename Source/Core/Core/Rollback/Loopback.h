// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Test only: a real Orca session against a virtual remote player in the same process. A plain run
// records every port's pad and the RAM checksum per frame (YG_PADREC); a second run replays that
// recording through Orca::Net::Session with RingPort, the remote seat's inputs arriving late, so
// the session predicts, mispredicts and rolls back. The remote also sends the first run's
// checksums: any difference from the plain run ends the session as a desync.

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Session.h"

namespace Rollback::Loopback
{
// One frame of a plain run: every port's pad in wire form, and the RAM checksum at its start.
struct RecordedFrame
{
  Orca::Net::Pads pads{};
  u64 checksum = 0;
};

// One line per frame: "<frame> <pad 1> <pad 2> <pad 3> <pad 4> <checksum>", pads as 16 hex digits.
std::string FormatRecordedFrame(int frame, const Orca::Net::Pads& pads, u64 checksum);
// Frames 0..n-1 of a recording; empty if the file is missing or a frame is absent.
std::vector<RecordedFrame> LoadRecording(const std::string& path);

// A remote seat replaying a recording. Its clock moves one tick per first-pass frame and per stall
// retry (Tick), never on re-runs; at tick t it has sent its inputs up to frame
// t - lag + input_delay, where lag is `lag` plus a deterministic 0..`jitter`. Every
// `checksum_every` frames it sends the recording's checksum for a frame it has reached.
class VirtualRemote final : public Orca::Net::Transport
{
public:
  VirtualRemote(const std::vector<RecordedFrame>& recording, int seat, int local_seat, int lag,
                int jitter, int input_delay, int checksum_every);

  void Tick() { ++m_ticks; }
  // Every recorded input has been sent: a session still waiting for more ran past the recording.
  bool Exhausted() const { return m_delivered >= static_cast<int>(m_recording.size()) - 1; }

  void Send(const Orca::Net::Packet& packet) override;
  std::vector<Orca::Net::Packet> Receive() override;
  bool Connected() const override { return true; }

private:
  int Lag() const;

  const std::vector<RecordedFrame>& m_recording;
  int m_seat;
  int m_local_seat;
  int m_lag;
  int m_jitter;
  int m_input_delay;
  int m_checksum_every;
  int m_ticks = 0;
  int m_delivered = -1;       // newest frame whose input was sent
  int m_local_newest = -1;    // newest local frame received (acknowledged back)
  int m_last_checksum = 0;
};
}  // namespace Rollback::Loopback
