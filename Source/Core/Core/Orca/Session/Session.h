// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

// Orca's rollback session: the input exchange and the decisions about which frame the emulator runs
// next. It knows nothing about Dolphin; the emulator side implements Game, the network side
// Transport. The emulator calls OnBoundary at every frame boundary (after a frame completes) and
// does what the returned Step says.
namespace Orca::Net
{
// One player's controller for one frame, as the SI device reports it (opaque to the session).
using Pad = std::array<u8, 8>;
constexpr int MAX_SEATS = 4;
// Bounds on what a peer may send: inputs per packet, and how far ahead of us its frames may be (a
// peer is never more than its input delay plus a rollback window ahead; this is generous).
constexpr int MAX_PADS_IN_PACKET = 64;
constexpr int MAX_FRAMES_AHEAD = 240;
using Pads = std::array<Pad, MAX_SEATS>;

// What the session needs from the emulator. Frame numbers count game frames from 0. "The state at
// the start of frame f" is what the emulator holds at the boundary after frame f-1 completed.
class Game
{
public:
  virtual ~Game() = default;
  // Snapshot the current state as the state at the start of `frame`. Only frames the session may
  // roll back to, or compare checksums of, are saved: Load is never asked for an unsaved frame.
  virtual bool Save(int frame) = 0;
  // Restore the state at the start of `frame`; the next boundary reached is the end of `frame`.
  virtual bool Load(int frame) = 0;
  // The pads every port reports while `frame` runs.
  virtual void SetPads(int frame, const Pads& pads) = 0;
  // True while frames that were already shown are re-run (the emulator skips rendering).
  virtual void SetResimulating(bool resimulating) = 0;
  // A hash of the saved state at the start of `frame`, for desync detection (nullopt if not saved).
  // Only asked for every Config::checksum_every frames; it may be computed at Save time.
  virtual std::optional<u64> SnapshotChecksum(int frame) = 0;
  // After a stall: restart frame pacing from now, so the emulator doesn't race to make up the time.
  virtual void ResetPacing() = 0;
};

// One message from one player: its most recent inputs, what it has received, and a checksum.
struct Packet
{
  int seat = 0;
  // pads[i] is the input for frame first_frame + i.
  int first_frame = 0;
  std::vector<Pad> pads;
  // Highest frame of each other seat's input this sender has received in order (-1: none).
  std::array<int, MAX_SEATS> ack{-1, -1, -1, -1};
  // The frame this sender is at, for time sync.
  int current_frame = 0;
  std::optional<int> checksum_frame;
  u64 checksum = 0;
};

class Transport
{
public:
  virtual ~Transport() = default;
  virtual void Send(const Packet& packet) = 0;
  // Packets received since the last call, in arrival order.
  virtual std::vector<Packet> Receive() = 0;
  virtual bool Connected() const = 0;
};

struct Config
{
  int seats = 2;
  int local_seat = 0;
  // Frames between sampling a local input and the frame it applies to.
  int input_delay = 2;
  // Deepest rollback allowed; beyond it the session stalls until inputs arrive.
  int max_rollback = 7;
  // Every this many confirmed frames, players exchange a state checksum (a full RAM hash costs a few
  // milliseconds, so not every frame).
  int checksum_every = 60;
  // Most inputs repeated in one packet.
  int max_pads_per_packet = 16;
};

enum class StepKind
{
  // Run the next frame (pads have been set).
  Run,
  // A rollback started: the state was loaded and the next frame to run has its pads set.
  Rollback,
  // Waiting for remote inputs: call OnBoundary again with the same frame without running anything
  // (after a short wait; give up if the emulator is stopping).
  Stall,
  // The session ended (see Session::Error).
  Ended,
};

struct Step
{
  StepKind kind = StepKind::Run;
  int frame = 0;  // the frame that runs next (Run, Rollback), or the stalled frame
};

struct Stats
{
  int rollbacks = 0;
  int resimulated_frames = 0;
  int deepest_rollback = 0;
  int stalls = 0;
  int checksums_matched = 0;
  int confirmed_frame = -1;
};

class Session
{
public:
  Session(const Config& config, Game& game, Transport& transport);

  // Called at every boundary. `completed_frame` is the frame that just finished (-1 at the very
  // first boundary, before frame 0). `local_pad` is the local controller now; it applies to frame
  // (next frame + input_delay). Re-run boundaries also call this; the pad is ignored there.
  Step OnBoundary(int completed_frame, const Pad& local_pad);

  const std::string& Error() const { return m_error; }
  const Stats& GetStats() const { return m_stats; }
  int CurrentFrame() const { return m_frame; }
  bool Resimulating() const { return m_resim_until >= 0; }

private:
  void ReceivePackets();
  void SendPacket();
  Pads PadsFor(int frame);
  int ConfirmedFrame() const;
  bool NeedsSnapshot(int frame) const;
  void CheckChecksums();
  Step Fail(std::string error);

  Config m_config;
  Game& m_game;
  Transport& m_transport;

  // The next frame to run in normal flow.
  int m_frame = 0;
  // While re-running: the last frame of the re-run (the newest frame already run before it).
  int m_resim_until = -1;
  // Earliest frame that ran with a wrong guess (INT_MAX: none).
  int m_dirty;

  std::map<int, Pad> m_local;
  std::array<std::map<int, Pad>, MAX_SEATS> m_remote;
  // Highest frame of each seat's input received contiguously.
  std::array<int, MAX_SEATS> m_received{-1, -1, -1, -1};
  // The remote pads each frame actually ran with.
  std::map<int, Pads> m_used;
  // Highest of our own frames each seat has acknowledged.
  std::array<int, MAX_SEATS> m_acked{-1, -1, -1, -1};
  std::array<int, MAX_SEATS> m_remote_frame{0, 0, 0, 0};

  std::map<int, u64> m_local_checksums;
  std::map<int, u64> m_peer_checksums;
  int m_last_checksum_frame = 0;
  std::optional<int> m_pending_checksum;
  int m_last_sent_newest = -1;
  bool m_stalling = false;

  Stats m_stats;
  std::string m_error;
};
}  // namespace Orca::Net
