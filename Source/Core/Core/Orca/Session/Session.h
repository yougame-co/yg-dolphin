// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <climits>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
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
// Not a button: the top bit of the first byte (outside PadCodec's PAD_WIRE_BUTTONS, so a sampled pad
// never has it) marks a port with no controller plugged in. Drop-in play leaves every port without
// a player unplugged; a friend who joins plugs in at a frame every machine agrees on.
constexpr u8 PAD_WIRE_UNPLUGGED = 0x80;
constexpr Pad UNPLUGGED_PAD{PAD_WIRE_UNPLUGGED, 0, 0, 0, 0, 0, 0, 0};
inline bool IsUnplugged(const Pad& pad)
{
  return (pad[0] & PAD_WIRE_UNPLUGGED) != 0;
}
// A frame that never comes (about 200 days of play): a seat plugged in "at NEVER" isn't plugged
// in. Not INT_MAX: the session runs on Dolphin's CPU thread, where the JIT leaves FPCR.AH set on
// Apple Silicon, and clang builds a {0, INT_MAX} pair as movi + FNEG, which FPCR.AH turns into
// {0, -1} (FNEG then leaves a NaN's sign alone). 0x3fffffff pairs are plain MOV immediates.
constexpr int NEVER = 0x3fffffff;
// Bounds on what a peer may send: inputs per packet, and how far ahead of us its frames may be (a
// peer is never more than its input delay plus a rollback window ahead; this is generous).
constexpr int MAX_PADS_IN_PACKET = 64;
constexpr int MAX_FRAMES_AHEAD = 240;
// Drop-in history frames one packet may carry (a sender sends half, so two coalesced still fit).
constexpr int MAX_HISTORY_IN_PACKET = 512;
// Most predicted frames between two snapshots (Config::snapshot_every). A rollback may then load a
// snapshot up to MAX_SNAPSHOT_EVERY - 1 frames older than the first wrong frame and re-run from
// there.
constexpr int MAX_SNAPSHOT_EVERY = 4;
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
  // What one Save costs on this machine, smoothed, in milliseconds (0: not measured). A full
  // snapshot copies all of RAM: about 1.7 ms on an M-series Mac, 9 ms or more on a PC with older
  // DDR4, where the copy also evicts the L3 the next frame needs. Picks Config::snapshot_every.
  virtual double SaveMs() const { return 0; }
};

// Drop-in: a seat's controller is plugged in for frames [plug_from, unplug_from), and the seat
// exchanges live inputs from live_from on (before that, its player replays the host's history).
// NEVER for a frame that hasn't been decided.
struct SeatPlan
{
  int plug_from = NEVER;
  int unplug_from = NEVER;
  int live_from = NEVER;
  bool operator==(const SeatPlan&) const = default;
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
  // Frames the sender re-simulated in its last one-second window, counted from each rollback's
  // first wrong frame (-1: no window yet). A side's rollbacks come from the other side's input
  // delay, so each side adapts on the worst number.
  int resimulated = -1;
  // The sender's stalls that count for the delay, over the whole match (-1: not reported): once
  // every peer's inputs were arriving, and not lined up with a hitch on either machine. Each side's
  // stalls come from the others' input delay, so every side adapts on all of them; a running count
  // can't be missed or counted twice.
  int stalls = -1;
  // The sender's hitches (Session::OnLocalHitch) over the whole match (-1: not reported).
  int hitches = -1;
  // The sender's smoothed lead over the slowest other player, in tenths of a frame: its frame minus
  // the frame that player last reported (nullopt: nothing received yet). Both leads include the
  // same transit time, so time sync halves their difference and the transit cancels out.
  std::optional<int> advantage;
  // The sender's smoothed round trip to the relay, in milliseconds (nullopt: none measured). The
  // one-way time between two players is half of each one's round trip to the relay.
  std::optional<int> rtt;
  std::optional<int> checksum_frame;
  u64 checksum = 0;

  // Drop-in (see SeatPlan): which seats are plugged in when. Only the host's roster counts.
  std::optional<std::array<SeatPlan, MAX_SEATS>> roster;
  // The highest frame of the shared history this sender holds, contiguously from frame 0 (-1:
  // none). The host sends a joining player the history it lacks.
  int history_ack = -1;
  // History for `history_seat` (-1: none): every seat's pads for frames history_first onwards,
  // from the host's log of confirmed frames.
  int history_seat = -1;
  int history_first = 0;
  std::vector<Pads> history;
  // This player asks to unplug (leave); the host picks the frame.
  bool leaving = false;
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
  // Frames between sampling a local input and the frame it applies to: the starting delay and the
  // floor of the adaptive one.
  int input_delay = 2;
  // Ceiling of the adaptive delay (100 ms at 60 Hz). At or below input_delay the delay never
  // changes. Each player's delay is its own: every local input travels explicitly, whichever
  // boundary sampled it, so peers with different delays agree on every frame.
  int max_delay = 6;
  // Game frames per second. Frames are the session's only clock: "a second" for the delay and
  // time-sync windows is this many frames run, so sessions fed the same packets decide alike. Also
  // converts round trips in milliseconds into frames.
  int frame_rate = 60;
  // Ask for a one-frame wait (StepKind::Wait) while this player runs ahead of the others (needs
  // packets from a peer whose session reports its lead).
  bool time_sync = true;
  // Deepest rollback allowed; beyond it the session stalls until inputs arrive.
  int max_rollback = 7;
  // Every this many confirmed frames, players exchange a state checksum (a full RAM hash costs a few
  // milliseconds, so not every frame).
  int checksum_every = 60;
  // Most inputs repeated in one packet.
  int max_pads_per_packet = 16;
  // Predicted frames per snapshot, 1 to MAX_SNAPSHOT_EVERY (0: from Game::SaveMs, re-chosen once a
  // "second": 1 while a save costs under about 3 ms, then one more per 3 ms). A predicted frame is saved
  // when it is k frames past the newest snapshot; checksum frames always are. A rollback loads the
  // newest snapshot at or before the first wrong frame and re-runs from there: up to k - 1 more
  // re-run frames (which skip rendering) in exchange for k times fewer snapshots.
  int snapshot_every = 0;

  // Drop-in play. Defaults: every seat below `seats` plugged in and live from frame 0 (a match both
  // players start together), with nothing logged.
  //
  // The frame the session starts at: the host's next frame when a friend arrives (the frames
  // before it are in the log), 0 for a friend who replays the host's game from the boot.
  int start_frame = 0;
  // The local pad the host held when the session started: it fills the frames the input delay
  // skips, so a held button stays held.
  Pad start_pad{};
  // Each seat's plan at the start (default: seats below `seats` plugged in from frame 0).
  std::optional<std::array<SeatPlan, MAX_SEATS>> plan;
  // The seat whose roster decides who plugs in and when (the host), or -1 (a fixed roster).
  int authority_seat = -1;
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
  // Time sync: this player has been running ahead of the others, so it gives one frame of time
  // back. Hold for one frame period, then call OnBoundary again with the same frame (the session
  // resets pacing there, so the frame is not made up). At most one per 30 frames.
  Wait,
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
  // Frames re-run, and the most in one rollback: from the snapshot loaded to the newest frame, so
  // they include the frames before the first wrong one that sparse snapshots re-run.
  int resimulated_frames = 0;
  int deepest_rollback = 0;
  // Snapshots taken, and the predicted frames per snapshot now (Config::snapshot_every).
  int saves = 0;
  int snapshot_every = 1;
  int stalls = 0;
  int checksums_matched = 0;
  int confirmed_frame = -1;
  // The current input delay and how it got there (start raises from round trips not counted).
  int delay = 0;
  int delay_raises = 0;
  int delay_lowers = 0;
  int delay_reverts = 0;
  // Time-sync waits, and the last averaged (our lead - their lead) / 2, in frames.
  int waits = 0;
  double frame_advantage = 0;
  // Slow frames this machine reported (OnLocalHitch), and the most any other player reported.
  int hitches = 0;
  int peer_hitches = 0;
  // Stalls that counted toward a delay raise (no hitch on either machine around them).
  int counted_stalls = 0;
};

class Session
{
public:
  Session(const Config& config, Game& game, Transport& transport);

  // Called at every boundary. `completed_frame` is the frame that just finished (-1 at the very
  // first boundary, before frame 0). `local_pad` is the local controller now; it applies to frame
  // (next frame + Delay()). Re-run boundaries also call this; the pad is ignored there.
  Step OnBoundary(int completed_frame, const Pad& local_pad);
  // A measured round trip to the relay, from the transport's pings. Call it on the thread that
  // calls OnBoundary, once per new measurement (YouGameRoom::RoundTripMs gives a sequence number
  // that changes with each pong; feed only when it changed, or one ping counts many times). In
  // the first three seconds it sets the delay to cover what the rollback window can't.
  void OnRoundTrip(int rtt_ms);
  // This machine just had a slow frame (a disc read, a shader compile: more than two frame periods
  // of wall time outside the session's own stalls and waits). The session has no wall clock, so
  // only the emulator can say; peers hear of it and leave the stalls it caused out of the delay.
  void OnLocalHitch();

  // ---- Drop-in ----
  // The confirmed history: every seat's pads for frames [base, base + size). The host starts its
  // session with the log of its solo frames since its keyframe and sends a joining player the part
  // it lacks; TakeLog hands it back (with every frame run so far) when the session ends.
  void SetLog(int base, std::vector<Pads> log);
  std::vector<Pads> TakeLog(int* base);
  int LogEnd() const { return m_log_base + static_cast<int>(m_log.size()); }
  // Host: a player at `seat` arrived and loads the keyframe of frame `history_from`; it replays the
  // log from there up to live_from - 1 and exchanges live inputs from live_from on. The session
  // plugs its controller in once it has caught up.
  void AddPeer(int seat, int live_from, int history_from);
  // Host: the player at `seat` is gone (it left the room or stopped answering): its controller
  // unplugs after the last input received from it.
  void DropSeat(int seat);
  // A joining player: ask the host to unplug this controller. LeftDone() once it has.
  void RequestLeave();
  bool LeftDone() const;
  // Host: no other player is plugged in or joining: the session can end and the host play solo.
  bool Idle() const;
  // The local inputs already sampled for frames from CurrentFrame() on (the host keeps them when it
  // goes back to solo play).
  std::vector<Pad> PendingLocal() const;
  // Whether this machine's own controller is plugged in at `frame`.
  bool Plugged(int seat, int frame) const;
  // The newest frame the host reported (a joining player catches up to it).
  int AuthorityFrame() const;
  // This player is still replaying the host's game: its own controller isn't plugged in yet.
  bool CatchingUp() const;
  // Host: drop every other player (they stopped answering, or the session failed on them).
  void DropAllPeers();
  // Host: a keyframe of the state at the start of CurrentFrame() would be final: every earlier
  // frame is confirmed and nothing is being re-run.
  bool Settled() const;
  const std::array<SeatPlan, MAX_SEATS>& Plan() const { return m_plan; }
  // The drop-in bookkeeping in one line, for logs: per seat its plan, received, acknowledged,
  // reported frame and history acknowledgement; the log and local input ranges.
  std::string Describe() const;

  const std::string& Error() const { return m_error; }
  const Stats& GetStats() const { return m_stats; }
  int CurrentFrame() const { return m_frame; }
  bool Resimulating() const { return m_resim_until >= 0; }
  int Delay() const { return m_delay; }

private:
  void ReceivePackets();
  void SendPacket();
  Pads PadsFor(int frame);
  int ConfirmedFrame() const;
  bool NeedsSnapshot(int frame) const;
  void ChooseSnapshotEvery();
  void CheckChecksums();
  Step Fail(std::string error);
  bool SaveOnce(int frame);
  void SampleLocal(const Pad& local_pad);
  void AdaptDelay();
  int LatencyDelay() const;
  int DelayFloor() const;
  void ApplyStartDelay();
  double FramesFromMs(double ms) const;
  void TrackAdvantage();
  int KnownThrough(int seat) const;
  bool InPlay(int seat) const;
  void AdvanceReceived(int seat);
  void ApplyRoster(const std::array<SeatPlan, MAX_SEATS>& roster);
  void ReceiveHistory(const Packet& packet);
  void AppendConfirmedToLog();
  void PlugCaughtUpPeers();
  void AddHistory(Packet* packet);

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
  // The frame whose state was last saved since the last load (-1: none): a repeated boundary on a
  // stalled or waiting frame must not snapshot it again.
  int m_last_saved = -1;
  // Frames whose snapshot holds the state this session's history leads to: a rollback forgets
  // those after the frame it loads, until the re-run saves them again.
  std::set<int> m_saved;
  // Predicted frames per snapshot (Config::snapshot_every, or chosen from Game::SaveMs).
  int m_snapshot_every = 1;

  // Adaptive input delay, decided once per window of frame_rate frames.
  int m_delay;
  int m_window_start = 0;
  // Frames re-run from the first wrong one on, over the match: what the link costs, whatever the
  // snapshot spacing adds before it. The delay adapts on these.
  int m_guessed_frames = 0;
  int m_window_resimulated = 0;
  // Re-simulated frames in this player's last window (sent), and in each peer's (received).
  int m_last_resimulated = -1;
  std::array<int, MAX_SEATS> m_peer_resimulated{-1, -1, -1, -1};
  // Stalls that count for the delay (sent), as of the last window; each peer's reported count, and
  // how much of it the last window already acted on.
  int m_link_stalls = 0;
  int m_window_link_stalls = 0;
  std::array<int, MAX_SEATS> m_peer_stalls{0, 0, 0, 0};
  std::array<int, MAX_SEATS> m_peer_stalls_seen{0, 0, 0, 0};
  // Counted stalls, here and reported, in each of the last STALL_WINDOWS windows (oldest first).
  std::array<int, 3> m_stall_history{};
  // The frame the current stall began at, and whether every peer's inputs had started arriving.
  int m_stall_frame = 0;
  bool m_stall_after_input = false;
  // This machine's hitches (sent), each peer's, and the frame the latest of any was heard of.
  int m_hitches = 0;
  std::array<int, MAX_SEATS> m_peer_hitches{0, 0, 0, 0};
  int m_last_hitch_frame = INT_MIN / 2;
  // Windows left before a delay raised for stalls may be lowered again.
  int m_stall_hold = 0;
  int m_quiet_windows = 0;
  int m_quiet_needed;
  // After a lower: the delay before it, restored if a window on probation is not quiet (0: none).
  int m_tried_delay = 0;
  int m_probe_windows = 0;

  // Smoothed round trip to the relay and its mean deviation, in milliseconds (no samples yet:
  // nullopt), and each peer's as it reported.
  std::optional<double> m_rtt_ms;
  double m_rtt_dev_ms = 0;
  std::array<std::optional<int>, MAX_SEATS> m_peer_rtt;
  int m_rtt_samples = 0;
  // The delay the start estimate last set (0: none); it may follow the estimate down only while
  // nothing else changed the delay since.
  int m_start_delay = 0;

  // Time sync, per peer, in frames: our latest lead over it, that lead smoothed, and its own
  // smoothed lead over its slowest peer as it reported. The balance (ours - theirs) / 2 is averaged
  // over a window.
  std::array<std::optional<double>, MAX_SEATS> m_advantage;
  std::array<std::optional<double>, MAX_SEATS> m_smoothed_advantage;
  std::array<std::optional<double>, MAX_SEATS> m_remote_advantage;
  double m_balance_sum = 0;
  int m_balance_samples = 0;
  int m_sync_start = 0;
  bool m_wait_pending = false;
  bool m_waiting = false;

  // Drop-in.
  std::array<SeatPlan, MAX_SEATS> m_plan{};
  int m_log_base = 0;
  std::vector<Pads> m_log;
  // Host: per seat, the history frames sent (and when the peer's acknowledgement last moved), and
  // the newest history frame the peer acknowledged.
  std::array<int, MAX_SEATS> m_history_sent{-1, -1, -1, -1};
  std::array<int, MAX_SEATS> m_history_acked{-1, -1, -1, -1};
  std::array<int, MAX_SEATS> m_history_stuck{0, 0, 0, 0};
  bool m_leaving = false;

  Stats m_stats;
  std::string m_error;
};
}  // namespace Orca::Net
