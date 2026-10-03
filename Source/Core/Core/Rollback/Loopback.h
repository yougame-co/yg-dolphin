// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Test only: a real Orca session against a virtual remote player in the same process. A plain run
// records every port's pad and the RAM checksum per frame (YG_PADREC); a second run replays that
// recording through Orca::Net::Session with RingPort, the remote seat's inputs arriving late, so
// the session predicts, mispredicts and rolls back. The remote also sends the first run's
// checksums: any difference from the plain run ends the session as a desync.

#pragma once

#include <chrono>
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

// Remote seats replaying a recording. Their clock moves one tick per first-pass frame and per
// stall retry (Tick), never on re-runs; at tick t a seat has sent its inputs up to frame
// t - lag + input_delay, where lag is `lag` plus a deterministic 0..`jitter` that differs per seat.
// Every `checksum_every` frames each seat sends the recording's checksum for a frame it reached.
class VirtualRemote final : public Orca::Net::Transport
{
public:
  VirtualRemote(const std::vector<RecordedFrame>& recording, std::vector<int> seats, int local_seat,
                int lag, int jitter, int input_delay, int checksum_every);

  // A first-pass frame of this player: the remote's clock moves a tick (after link spikes, it
  // first waits out the lead it took, as time sync would make it).
  void Tick();
  // A retry while this player stalls: the remote keeps playing. Without spikes a tick per retry;
  // with them, a tick per frame period of wall time, at most max_lead ticks ahead of this player.
  void TickWhileWaiting();
  // From this frame on, every remote seat sends nothing more (a game that froze while its
  // connection stays open). Negative: never.
  void SilenceFrom(int frame) { m_silent_from = frame; }
  // Test only: from now on, every `period` of wall time the link holds every remote seat's packets
  // for `length` (a spike on the way to this player), then delivers them in one burst. Stalls
  // that long make this player catch up afterwards, as in a match.
  void StartSpikes(std::chrono::milliseconds period, std::chrono::milliseconds length,
                   int max_lead);
  int Spikes() const { return m_spikes; }
  // Every recorded input has been sent: a session still waiting for more ran past the recording.
  bool Exhausted() const;

  void Send(const Orca::Net::Packet& packet) override;
  std::vector<Orca::Net::Packet> Receive() override;
  bool Connected() const override { return true; }

private:
  struct Seat
  {
    int seat = 0;
    int delivered = -1;  // newest frame whose input was sent
    int last_checksum = 0;
  };
  int Lag(int seat) const;

  const std::vector<RecordedFrame>& m_recording;
  std::vector<Seat> m_seats;
  int m_local_seat;
  int m_lag;
  int m_jitter;
  int m_input_delay;
  int m_checksum_every;
  int m_ticks = 0;
  int m_silent_from = -1;
  int m_local_newest = -1;  // newest local frame received (acknowledged back)
  // Link spikes (StartSpikes): zero period means none.
  std::chrono::milliseconds m_spike_period{0};
  std::chrono::milliseconds m_spike_length{0};
  std::chrono::steady_clock::time_point m_spike_start;
  std::chrono::steady_clock::time_point m_last_tick;
  int m_max_lead = 0;
  int m_lead = 0;  // ticks taken while this player stalled, not yet matched by its frames
  bool m_spiking = false;
  int m_spikes = 0;
};
}  // namespace Rollback::Loopback
