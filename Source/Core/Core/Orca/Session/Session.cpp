// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/Session.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <utility>

#include <fmt/format.h>

namespace Orca::Net
{
namespace
{
// Input delay, as Slippi does it: two frames (Config::input_delay), and rollback hides the rest of
// the link, its spikes included. Delay costs every input its frames all the time; a rollback costs
// only the frames a late input changed, and only then; a stall costs only the frames it held. So
// the adaptive delay covers only what rollback can't, sustained, and never rises for anything
// rollback covered or for one spike past it:
//
// - The typical link (LatencyDelay): the median of this player's latest RTT_SAMPLES round trips to
//   the relay (one a second) and the slowest peer's median, as each reports it. A median ignores
//   spikes in up to half the window: a Mac's Wi-Fi going off channel every ten seconds or so
//   (AWDL, 35 ms pings with one of 115 ms) moves nothing, while a link that really got slower
//   moves it within RTT_SAMPLES / 2 seconds. Never a maximum, never a mean a spike drags up. The
//   delay rises to what the link needs as soon as both ends' medians are in (RaiseToLink).
// - Stalls the others had on this player's inputs, sustained: when they stalled in at least
//   RAISE_STALLED_WINDOWS of the last RAISE_WINDOWS windows, the delay rises a frame. Windows with
//   a stall, not stalls: a spike past the rollback window stalls once (a burst of them now and
//   then), and so does each of a spike every few seconds (a Mac's Wi-Fi on a bad day: 80-220 ms
//   every 2-5 s). That raises nothing, since a frame more would hide few of them and cost every
//   input a frame all match long; a link too slow for the delay stalls second after second. A
//   side stalls when the others' inputs outran its rollback window, so only the others' delays can
//   help it: each player raises its own delay on the others' stalls, never on its own, and only on
//   those of a player whose lag on its inputs (below) reached max_rollback then: with three or four
//   players, one that stalled on someone else's inputs is theirs to answer.
//   A stall counts only for the link (each side judges its own, Packet::stalls). Not one lined up
//   with a hitch on either machine (no delay cures a frozen emulator), nor one that came of the
//   other side's own stall (HITCH_FRAMES around a counted stall of theirs: it stopped sending new
//   inputs because it was stalled itself, so its delay could not have helped), nor any while a
//   controller isn't plugged in yet (a joining player runs to the host's newest inputs as fast as
//   it can, by design), nor in the SETTLE_SECONDS after the newest plug-in (a match's first
//   seconds, or a friend's drop-in: its catch-up leaves it behind the host, the host stalls on its
//   first inputs and time sync evens the two out), nor waiting for a peer's first packet (the
//   trip).
//
// Rollbacks don't move the delay at all, however many: at two frames a 40 ms link rolls back a
// frame or two now and then, and a spike deeper; that is rollback doing its job.
//
// A delay above the link's comes back down a frame per window: one the link put there once the
// link has wanted less for LINK_HOLD_WINDOWS (a first median inflated by a keyframe's transfer
// comes back down in seconds), one stalls put there once the others' stalls have stopped coming
// second after second for STALL_HOLD_WINDOWS (quiet windows: no stall, or the only one of the last
// STALL_WINDOWS with one; a spike now and then is no reason to keep a frame that could not hide
// it), then a frame each further quiet window. Either way only if the others would still have had
// a frame of room with a frame less. Their packets say how far each ran past the newest of this
// player's inputs it had (its frame minus its acknowledgement): as deep as a change of them would
// roll it back; past max_rollback it stalls.
// That plus the delay is how far behind the others' frame this player's sampling ran (its reach),
// whatever the delay: with d frames, they run reach - d past its inputs. A lower to d is allowed
// while no window of the last STALL_WINDOWS (one with a stall, or around a hitch, aside: a stall
// says nothing of how near the edge the rest of the window ran) reached max_rollback there. On a
// jittery link that may never come, and a step down there would only bring the stalls back. A
// raise for stalls within PROBATION_WINDOWS of a step down undoes it anyway, and doubles the quiet
// run the next try needs. A delay the player chose (Config::fixed_delay) never adapts.
constexpr int MIN_RTT_SAMPLES = 3;
constexpr int LINK_HOLD_WINDOWS = 3;
constexpr int EDGE_WINDOWS = 3;
constexpr int STALL_WINDOWS = 5;
constexpr int RAISE_WINDOWS = 4;
constexpr int RAISE_STALLED_WINDOWS = 3;
constexpr int QUIET_STALLED_WINDOWS = 1;
constexpr int STALL_HOLD_WINDOWS = STALL_WINDOWS;
constexpr int MAX_STALL_HOLD_WINDOWS = 8 * STALL_HOLD_WINDOWS;
constexpr int PROBATION_WINDOWS = 2 * STALL_WINDOWS;
// The first seconds after a plug-in (the match's start, a friend's drop-in): the game boots and
// loads from the disc, or a joiner that just caught up runs a dozen frames behind the host, which
// stalls on its first inputs until time sync evens them out. No stall counts until both settled.
constexpr int SETTLE_SECONDS = 10;
// What the link needs: an input reaches the other side one way after it was sampled, and runs at
// the boundary after that (up to a frame later); one more frame stays free for jitter.
constexpr int LATENCY_SLACK_FRAMES = 2;
// A stall that began within this many frames after a hitch on any machine (or during which one was
// heard of) is the hitch's, not the link's.
constexpr int HITCH_FRAMES = 30;
// No working relay is ten seconds away.
constexpr int MAX_RTT_MS = 10000;
// Time sync averages this many frames, waits at most once per that many, and only when it is more
// than this many frames ahead on average: anything less is jitter.
constexpr int TIME_SYNC_FRAMES = 30;
constexpr double TIME_SYNC_LEAD = 1.0;
// Snapshot spacing: one more predicted frame between snapshots per this much a save costs (about
// what a re-run frame without rendering costs on the slow PCs this is for), with a dead band
// around each step so a save time near one doesn't flip the spacing every window.
constexpr double SAVE_MS_PER_SPACING = 3.0;
constexpr double SPACING_DEAD_BAND_MS = 0.25;
// Drop-in. A joining player counts as caught up within this many frames (plus the round trip) of
// the host. History goes out at most this many frames per packet, in at most this many runs of
// equal pads (a packet stays a few KB), and at most this far past the frame the joiner reported. A
// history send the joiner hasn't acknowledged after this many packets goes again.
constexpr int CATCH_UP_SLACK = 8;
constexpr int MAX_HISTORY_RUNS = 32;
constexpr int HISTORY_AHEAD = 4096;
constexpr int HISTORY_RESEND_PACKETS = 120;
}  // namespace

Session::Session(const Config& config, Game& game, Transport& transport)
    : m_config(config), m_game(game), m_transport(transport), m_dirty(INT_MAX),
      m_delay(config.input_delay), m_hold_windows(STALL_HOLD_WINDOWS)
{
  m_config.max_delay = std::max(m_config.max_delay, m_config.input_delay);
  m_config.frame_rate = std::max(m_config.frame_rate, 1);
  m_config.snapshot_every = std::clamp(m_config.snapshot_every, 0, MAX_SNAPSHOT_EVERY);
  if (m_config.fixed_delay)
  {
    m_fixed_delay = std::clamp(*m_config.fixed_delay, std::min(1, m_config.max_delay),
                               m_config.max_delay);
    m_delay = *m_fixed_delay;
  }
  m_stats.delay = m_delay;
  m_stats.delay_fixed = m_fixed_delay.has_value();
  ChooseSnapshotEvery();
  m_config.seats = std::clamp(m_config.seats, 1, MAX_SEATS);
  if (m_config.plan)
  {
    m_plan = *m_config.plan;
  }
  else
  {
    for (int s = 0; s < m_config.seats; ++s)
      m_plan[s] = {0, NEVER, 0};
  }
  m_frame = std::max(m_config.start_frame, 0);
  m_log_base = m_frame;
  m_window_start = m_frame;
  m_sync_start = m_frame;
  m_last_checksum_frame = m_frame - m_frame % m_config.checksum_every;
  // Frames before the input delay have no sampled input: both sides treat them as neutral (a host
  // whose friend just arrived: as the pad it held), and sending them explicitly lets the other side
  // confirm them like any other frame.
  if (Plugged(m_config.local_seat, m_frame))
  {
    for (int f = m_frame; f < m_frame + m_delay; ++f)
      m_local[f] = m_config.start_pad;
  }
  for (int s = 0; s < m_config.seats; ++s)
    AdvanceReceived(s);
}

bool Session::Plugged(int seat, int frame) const
{
  return seat >= 0 && seat < MAX_SEATS && frame >= m_plan[seat].plug_from &&
         frame < m_plan[seat].unplug_from;
}

bool Session::RerunFrom(int frame)
{
  if (frame >= m_frame)
    return true;  // nothing ran yet
  // As a rollback to `frame` would (OnBoundary): the newest snapshot at or before it, within the
  // window. Frames that ran confirmed have none (only predicted and checksum frames are saved).
  const auto after = m_saved.upper_bound(frame);
  const int from = after == m_saved.begin() ? -1 : *std::prev(after);
  if (m_frame - frame > m_config.max_rollback || from < 0 ||
      m_frame - from > m_config.max_rollback + MAX_SNAPSHOT_EVERY - 1)
  {
    return false;
  }
  m_dirty = std::min(m_dirty, frame);
  return true;
}

int Session::SettleFrom() const
{
  // The newest plug-in of this player's controller or of any other player's still in play.
  int newest = INT_MIN / 2;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if ((s == m_config.local_seat || InPlay(s)) && m_plan[s].plug_from != NEVER)
      newest = std::max(newest, m_plan[s].plug_from);
  }
  return newest + SETTLE_SECONDS * m_config.frame_rate;
}

bool Session::InPlay(int seat) const
{
  return seat != m_config.local_seat && seat >= 0 && seat < m_config.seats &&
         m_plan[seat].live_from != NEVER;
}

bool Session::CatchingUp() const
{
  return m_config.authority_seat >= 0 && m_config.authority_seat != m_config.local_seat &&
         m_plan[m_config.local_seat].plug_from == NEVER;
}

int Session::AuthorityFrame() const
{
  const int a = m_config.authority_seat;
  return a >= 0 && a != m_config.local_seat ? m_remote_frame[a] : m_frame;
}

void Session::AdvanceReceived(int seat)
{
  int& received = m_received[seat];
  // The log holds every seat's pads, and a seat's pads before it plugs in are known: unplugged.
  received = std::max(received, LogEnd() - 1);
  if (m_plan[seat].plug_from != NEVER)
    received = std::max(received, m_plan[seat].plug_from - 1);
  while (m_remote[seat].contains(received + 1))
    ++received;
}

int Session::KnownThrough(int seat) const
{
  const SeatPlan& plan = m_plan[seat];
  // No live inputs (gone, or not here), or not plugged in yet: every frame's pad is known.
  if (plan.live_from == NEVER || plan.plug_from == NEVER)
    return NEVER;
  // Unplugged for good once the inputs before the unplug are in.
  if (plan.unplug_from != NEVER && m_received[seat] >= plan.unplug_from - 1)
    return NEVER;
  return m_received[seat];
}

std::string Session::Describe() const
{
  const auto n = [](int v) { return v == NEVER ? std::string("-") : std::to_string(v); };
  std::string out = fmt::format("frame {} confirmed {} log [{}, {}) local [{}, {}]", m_frame,
                                ConfirmedFrame(), m_log_base, LogEnd(),
                                m_local.empty() ? -1 : m_local.begin()->first,
                                m_local.empty() ? -1 : m_local.rbegin()->first);
  for (int s = 0; s < m_config.seats; ++s)
  {
    const SeatPlan& p = m_plan[s];
    out += fmt::format("; seat {} plug {} unplug {} live {} received {} acked {} at {} history {}/{}",
                       s + 1, n(p.plug_from), n(p.unplug_from), n(p.live_from), m_received[s],
                       m_acked[s], m_remote_frame[s], m_history_sent[s], m_history_acked[s]);
  }
  return out;
}

void Session::SetLog(int base, std::vector<Pads> log)
{
  m_log_base = base;
  m_log = std::move(log);
  for (int s = 0; s < m_config.seats; ++s)
    AdvanceReceived(s);
}

std::vector<Pads> Session::TakeLog(int* base)
{
  // Every frame that ran is final for the machine handing its log back (the host going solo, the
  // others dropped): their last run is what its state holds.
  while (LogEnd() < m_frame)
  {
    const auto it = m_used.find(LogEnd());
    if (it == m_used.end())
      break;
    m_log.push_back(it->second);
  }
  *base = m_log_base;
  return std::move(m_log);
}

void Session::AddPeer(int seat, int live_from, int history_from)
{
  if (seat < 0 || seat >= m_config.seats || seat == m_config.local_seat)
    return;
  SeatPlan& plan = m_plan[seat];
  plan = {NEVER, NEVER, live_from};
  m_acked[seat] = live_from - 1;
  m_received[seat] = -1;
  m_remote[seat].clear();
  m_remote_frame[seat] = 0;
  m_history_sent[seat] = m_history_acked[seat] = std::max(history_from, m_log_base) - 1;
  m_history_stuck[seat] = 0;
  m_peer_stalls[seat] = m_peer_stalls_seen[seat] = m_peer_hitches[seat] = 0;
  m_edge_frame[seat] = INT_MIN / 2;
  m_advantage[seat].reset();
  m_smoothed_advantage[seat].reset();
  m_remote_advantage[seat].reset();
  m_peer_rtt[seat].reset();
  AdvanceReceived(seat);
}

void Session::DropSeat(int seat)
{
  if (!InPlay(seat))
    return;
  SeatPlan& plan = m_plan[seat];
  if (plan.plug_from != NEVER)
  {
    // Its controller unplugs after the last input that came in (the stall rule kept this machine
    // within max_rollback of it, so a rollback reaches every frame that guessed otherwise).
    const int last = std::max(plan.plug_from, m_received[seat] + 1);
    if (last < plan.unplug_from)
    {
      plan.unplug_from = last;
      for (int f = last; f < m_frame; ++f)
      {
        if (const auto used = m_used.find(f); used != m_used.end() && !IsUnplugged(used->second[seat]))
          m_dirty = std::min(m_dirty, f);
      }
    }
  }
  plan.live_from = NEVER;
}

void Session::DropAllPeers()
{
  for (int s = 0; s < m_config.seats; ++s)
    DropSeat(s);
}

void Session::RequestLeave()
{
  m_leaving = true;
}

bool Session::LeftDone() const
{
  const int unplug = m_plan[m_config.local_seat].unplug_from;
  const int a = m_config.authority_seat;
  return unplug != NEVER && m_frame > unplug && a >= 0 && a != m_config.local_seat &&
         m_acked[a] >= unplug - 1;
}

bool Session::Idle() const
{
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (InPlay(s))
      return false;
  }
  return m_resim_until < 0 && m_dirty == INT_MAX;
}

bool Session::Settled() const
{
  return m_resim_until < 0 && m_dirty == INT_MAX && ConfirmedFrame() >= m_frame - 1;
}

std::vector<Pad> Session::PendingLocal() const
{
  std::vector<Pad> pending;
  for (auto it = m_local.lower_bound(m_frame); it != m_local.end(); ++it)
    pending.push_back(it->second);
  return pending;
}

void Session::ApplyRoster(const std::array<SeatPlan, MAX_SEATS>& roster)
{
  for (int s = 0; s < m_config.seats; ++s)
  {
    const SeatPlan& now = roster[s];
    SeatPlan& mine = m_plan[s];
    if (now == mine)
      continue;
    if (s == m_config.local_seat && now.plug_from != mine.plug_from && now.plug_from < m_frame)
    {
      Fail(fmt::format("The host plugged this controller in at frame {}, already past", now.plug_from));
      return;
    }
    // Frames that ran with this seat plugged in where it now isn't (or the reverse) run again.
    int changed = INT_MAX;
    if (now.plug_from != mine.plug_from)
      changed = std::min(now.plug_from, mine.plug_from);
    if (now.unplug_from != mine.unplug_from)
      changed = std::min({changed, now.unplug_from, mine.unplug_from});
    if (changed < m_frame && changed >= LogEnd())
      m_dirty = std::min(m_dirty, changed);
    if (s != m_config.local_seat && mine.live_from == NEVER && now.live_from != NEVER)
      m_acked[s] = now.live_from - 1;
    mine = now;
    AdvanceReceived(s);
  }
}

void Session::ReceiveHistory(const Packet& packet)
{
  if (packet.history_first > LogEnd() || packet.history.size() > MAX_HISTORY_IN_PACKET)
    return;  // a gap: the host sends it again
  for (size_t i = 0; i < packet.history.size(); ++i)
  {
    const int f = packet.history_first + static_cast<int>(i);
    const Pads& pads = packet.history[i];
    if (f < m_log_base)
      continue;
    if (f < LogEnd())
    {
      if (m_log[f - m_log_base] != pads)
      {
        Fail(fmt::format("The host's history changed at frame {}", f));
        return;
      }
      continue;
    }
    if (const auto used = m_used.find(f); used != m_used.end() && used->second != pads)
      m_dirty = std::min(m_dirty, f);
    m_log.push_back(pads);
  }
  for (int s = 0; s < m_config.seats; ++s)
    AdvanceReceived(s);
}

void Session::AppendConfirmedToLog()
{
  const int final_through = std::min(ConfirmedFrame(), m_frame - 1);
  while (LogEnd() <= final_through && LogEnd() < m_dirty)
  {
    const auto it = m_used.find(LogEnd());
    if (it == m_used.end())
      break;
    m_log.push_back(it->second);
  }
}

void Session::PlugCaughtUpPeers()
{
  // The host plugs a joining player's controller in once it has replayed the history and caught
  // up, far enough ahead that every machine hears of it before running that frame: the joiner may
  // be up to its delay plus a rollback window ahead of what the host last heard from it.
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (!InPlay(s))
      continue;
    SeatPlan& plan = m_plan[s];
    if (plan.plug_from != NEVER || m_history_acked[s] < plan.live_from - 1)
      continue;
    const int rtt_frames = RoundTripFrames(s);
    if (m_remote_frame[s] < m_frame - CATCH_UP_SLACK - rtt_frames)
      continue;
    plan.plug_from = m_frame + m_config.max_rollback + 2 * m_config.max_delay + rtt_frames + 4;
    AdvanceReceived(s);
  }
}

Step Session::Fail(std::string error)
{
  if (m_error.empty())
    m_error = std::move(error);
  if (m_resim_until >= 0)
  {
    m_resim_until = -1;
    m_game.SetResimulating(false);
  }
  return {StepKind::Ended, m_frame};
}

Step Session::OnBoundary(int completed_frame, const Pad& local_pad)
{
  if (!m_error.empty())
    return {StepKind::Ended, m_frame};
  // A re-run needs nothing from the transport: a connection lost mid re-run fails the session at
  // the next normal boundary, where the emulator's frame is the session's own.
  if (!m_transport.Connected() && m_resim_until < 0)
    return Fail("Connection lost");

  // A re-run frame just completed.
  if (m_resim_until >= 0)
  {
    const int next = completed_frame + 1;
    if (NeedsSnapshot(next) && !SaveOnce(next))
      return Fail(fmt::format("Saving frame {} failed", next));
    if (completed_frame < m_resim_until)
    {
      const Pads pads = PadsFor(next);
      m_used[next] = pads;
      m_game.SetPads(next, pads);
      return {StepKind::Run, next};
    }
    // Caught up with the newest frame that had already run: back to normal flow. This boundary's
    // local input was sampled and sent before the rollback started.
    m_resim_until = -1;
    m_game.SetResimulating(false);
  }
  else
  {
    if (completed_frame != m_frame - 1)
    {
      return Fail(fmt::format("Frame alignment lost: frame {} ended, expected {}", completed_frame,
                              m_frame - 1));
    }
    if (NeedsSnapshot(m_frame) && !SaveOnce(m_frame))
      return Fail(fmt::format("Saving frame {} failed", m_frame));

    // Running ahead of the others: give one frame of time back, deliberately and before it costs
    // a stall here and deep rollbacks there. Decided before sampling, so the frame's input is the
    // pad sampled after the wait, not one held for a frame.
    if (m_wait_pending)
    {
      m_wait_pending = false;
      m_waiting = true;
      ++m_stats.waits;
      return {StepKind::Wait, m_frame};
    }
    SampleLocal(local_pad);
  }

  ReceivePackets();
  if (!m_error.empty())
    return {StepKind::Ended, m_frame};
  CheckChecksums();
  if (!m_error.empty())
    return {StepKind::Ended, m_frame};
  if (m_config.authority_seat == m_config.local_seat)
    PlugCaughtUpPeers();
  SendPacket();

  // A frame that already ran with a wrong guess: load the newest snapshot at or before it and
  // re-run from there up to the newest frame. Frames before the wrong one re-run with the inputs
  // they ran with, so they come out the same.
  if (m_dirty < m_frame)
  {
    const int dirty = m_dirty;
    m_dirty = INT_MAX;
    if (m_frame - dirty > m_config.max_rollback)
      return Fail(fmt::format("Rollback of {} frames exceeds the limit", m_frame - dirty));
    // There is a snapshot at most k - 1 frames before any frame a rollback targets (NeedsSnapshot).
    const auto after = m_saved.upper_bound(dirty);
    const int from = after == m_saved.begin() ? -1 : *std::prev(after);
    const int depth = m_frame - from;
    if (from < 0 || depth > m_config.max_rollback + MAX_SNAPSHOT_EVERY - 1)
      return Fail(fmt::format("No snapshot to roll back to frame {} from", dirty));
    m_last_saved = -1;
    if (!m_game.Load(from))
      return Fail(fmt::format("Loading frame {} failed", from));
    // Snapshots after the loaded frame hold the history that is being replaced.
    m_saved.erase(m_saved.upper_bound(from), m_saved.end());
    ++m_stats.rollbacks;
    m_stats.resimulated_frames += depth;
    m_stats.deepest_rollback = std::max(m_stats.deepest_rollback, depth);
    m_guessed_frames += m_frame - dirty;
    m_resim_until = m_frame - 1;
    m_game.SetResimulating(true);
    const Pads pads = PadsFor(from);
    m_used[from] = pads;
    m_game.SetPads(from, pads);
    return {StepKind::Rollback, from};
  }

  // Too far ahead of the slowest player's inputs: wait without running anything. A player still
  // replaying the host's game runs only confirmed frames: guessing there would only roll back.
  if (m_frame - ConfirmedFrame() > (CatchingUp() ? 0 : m_config.max_rollback))
  {
    if (!m_stalling)
    {
      ++m_stats.stalls;
      m_stall_frame = m_frame;
      m_stall_after_input = ConfirmedFrame() >= 0 && Plugged(m_config.local_seat, m_frame) &&
                            m_frame >= SettleFrom();
    }
    m_stalling = true;
    return {StepKind::Stall, m_frame};
  }
  // A stall just ended: judged now, when the packet that ended it (and a hitch or a stall of the
  // other side's it reports) is in. Waiting for a peer's first packet is the trip, not the delay; a
  // joining player whose controller isn't plugged in yet stalls on the host's newest inputs by
  // design, and the seconds after a plug-in settle (SETTLE_SECONDS). The other side's stall, heard
  // of during this one or just before it began, is what stopped its new inputs: it ends with the
  // packet that ends this one (the first after it resumed).
  if (m_stalling && m_stall_after_input && m_last_hitch_frame < m_stall_frame - HITCH_FRAMES &&
      m_last_peer_stall_frame < m_stall_frame - HITCH_FRAMES)
  {
    m_stats.counted_stalls = ++m_link_stalls;
  }
  // After a stall or a wait, the time it held is not made up.
  if (m_stalling || m_waiting)
    m_game.ResetPacing();
  m_stalling = false;
  m_waiting = false;

  const int run = m_frame;
  // Every frame runs with this player's own input for it, the one the others receive.
  if (Plugged(m_config.local_seat, run) && run >= LogEnd() && !m_local.contains(run))
    return Fail(fmt::format("No local input for frame {}", run));
  const Pads pads = PadsFor(run);
  m_used[run] = pads;
  m_game.SetPads(run, pads);
  ++m_frame;
  if (m_frame - m_window_start >= m_config.frame_rate)
  {
    AdaptDelay();
    ChooseSnapshotEvery();
  }
  TrackAdvantage();

  AppendConfirmedToLog();
  // A player who unplugged and whose last inputs are in is gone (the host's call).
  if (m_config.authority_seat == m_config.local_seat)
  {
    for (int s = 0; s < m_config.seats; ++s)
    {
      const SeatPlan& plan = m_plan[s];
      if (InPlay(s) && plan.unplug_from != NEVER && m_frame > plan.unplug_from &&
          m_received[s] >= plan.unplug_from - 1)
      {
        m_plan[s].live_from = NEVER;
      }
    }
  }

  // History older than the deepest possible rollback (from a snapshot up to MAX_SNAPSHOT_EVERY - 1
  // frames before the first wrong frame) is no longer needed. Keep one remote input before the
  // window, since predictions repeat the last known input.
  const int oldest = m_frame - m_config.max_rollback - MAX_SNAPSHOT_EVERY - 1;
  int oldest_unacked = INT_MAX;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (InPlay(s))
      oldest_unacked = std::min(oldest_unacked, m_acked[s] + 1);
  }
  const int keep_local = std::min(oldest, oldest_unacked);
  std::erase_if(m_local, [keep_local](const auto& e) { return e.first < keep_local; });
  std::erase_if(m_used, [oldest](const auto& e) { return e.first < oldest; });
  m_saved.erase(m_saved.begin(), m_saved.lower_bound(oldest));
  for (int s = 0; s < m_config.seats; ++s)
  {
    auto& inputs = m_remote[s];
    while (inputs.size() > 1 && std::next(inputs.begin())->first < oldest)
      inputs.erase(inputs.begin());
  }
  std::erase_if(m_local_checksums, [this](const auto& e) { return e.first < m_frame - 600; });
  std::erase_if(m_peer_checksums, [this](const auto& e) { return e.first < m_frame - 600; });

  m_stats.confirmed_frame = ConfirmedFrame();
  return {StepKind::Run, run};
}

bool Session::SaveOnce(int frame)
{
  if (m_last_saved == frame)
    return true;
  if (!m_game.Save(frame))
    return false;
  m_last_saved = frame;
  m_saved.insert(frame);
  ++m_stats.saves;
  return true;
}

void Session::ChooseSnapshotEvery()
{
  // A save that costs more than a re-run frame is worth spreading out: one more predicted frame
  // between snapshots per SAVE_MS_PER_SPACING a save costs. Re-chosen once a window; any spacing
  // is safe to change to at any frame (NeedsSnapshot measures it from the newest snapshot, not
  // from frame numbers).
  if (m_config.snapshot_every > 0)
  {
    m_snapshot_every = m_config.snapshot_every;
  }
  else
  {
    const double save_ms = std::isfinite(m_game.SaveMs()) ? m_game.SaveMs() : 0;
    int& k = m_snapshot_every;
    while (k < MAX_SNAPSHOT_EVERY && save_ms > SAVE_MS_PER_SPACING * k + SPACING_DEAD_BAND_MS)
      ++k;
    while (k > 1 && save_ms < SAVE_MS_PER_SPACING * (k - 1) - SPACING_DEAD_BAND_MS)
      --k;
  }
  m_stats.snapshot_every = m_snapshot_every;
}

void Session::SampleLocal(const Pad& local_pad)
{
  const int target = m_frame + m_delay;
  // Drop-in: no input while this controller isn't plugged in (not yet, or no longer).
  const SeatPlan& plan = m_plan[m_config.local_seat];
  if (plan.plug_from == NEVER || target < plan.plug_from || target >= plan.unplug_from)
    return;
  const int newest = m_local.empty() ? plan.plug_from - 1 : m_local.rbegin()->first;
  // After a lower, the target already has an input (it went out with an earlier packet): this
  // sample is dropped, and the next boundary's lands on the frame after. A stalled or waiting
  // boundary repeats its frame and lands here too.
  if (target <= newest)
    return;
  // After a raise, the frames skipped over repeat the last input (a held direction stays held) and
  // travel like any other, so the other players can confirm them.
  const Pad held = m_local.empty() ? Pad{} : m_local.rbegin()->second;
  for (int f = newest + 1; f < target; ++f)
    m_local[f] = held;
  m_local[target] = local_pad;
}

void Session::OnRoundTrip(int rtt_ms)
{
  // Nothing a working relay measures; a broken clock or a ping answered after a freeze.
  if (rtt_ms < 0 || rtt_ms > MAX_RTT_MS)
    return;
  m_rtts.push_back(rtt_ms);
  if (static_cast<int>(m_rtts.size()) > RTT_SAMPLES)
    m_rtts.pop_front();
  const int count = static_cast<int>(m_rtts.size());
  if (count < MIN_RTT_SAMPLES)
    return;
  // The median (the lower one of an even count): what most round trips are, whatever the spikes.
  std::array<int, RTT_SAMPLES> sorted{};
  const auto end = std::copy(m_rtts.begin(), m_rtts.end(), sorted.begin());
  const auto middle = sorted.begin() + (count - 1) / 2;
  std::nth_element(sorted.begin(), middle, end);
  m_typical_rtt = *middle;
  m_stats.rtt_median = *m_typical_rtt;
  RaiseToLink();
}

void Session::RaiseToLink()
{
  // The typical link outgrew the rollback window plus the delay: up to what it needs, at once.
  // Down only in AdaptDelay, a frame at a time.
  if (m_fixed_delay || m_config.max_delay <= m_config.input_delay)
    return;
  const std::optional<int> link = LatencyDelay();
  if (!link || *link <= m_delay)
    return;
  const int from = m_delay;
  m_delay = *link;
  m_delay_from_link = true;
  ++m_stats.delay_raises;
  m_stats.delay = m_delay;
  NoteDelay(from, fmt::format("the link (round trips to the relay: median {} ms here, {} ms "
                              "there) needs {}",
                              m_stats.rtt_median, m_stats.peer_rtt_median, *link));
}

void Session::NoteDelay(int from, std::string why)
{
  if (m_delay_notes.size() >= MAX_DELAY_NOTES)
    m_delay_notes.erase(m_delay_notes.begin());
  m_delay_notes.push_back(
      fmt::format("input delay {} -> {} at frame {}: {}", from, m_delay, m_frame, why));
}

void Session::OnLocalHitch()
{
  ++m_hitches;
  m_stats.hitches = m_hitches;
  m_last_hitch_frame = m_frame;
}

void Session::SetFixedDelay(std::optional<int> frames)
{
  if (frames)
    frames = std::clamp(*frames, std::min(1, m_config.max_delay), m_config.max_delay);
  if (frames == m_fixed_delay)
    return;
  const int from = m_delay;
  m_fixed_delay = frames;
  if (m_fixed_delay)
  {
    m_delay = *m_fixed_delay;
  }
  else if (m_config.max_delay > m_config.input_delay)
  {
    // Adaptive again: from what the link needs, with no stalls on record and a fresh hold.
    m_delay = LatencyDelay().value_or(m_config.input_delay);
    m_delay_from_link = true;
    m_link_below_windows = 0;
    m_stall_history.fill(0);
    m_reach_history.fill(0);
    m_quiet_windows = 0;
    m_hold_windows = STALL_HOLD_WINDOWS;
    m_windows_since_lower = INT_MAX / 2;
  }
  else
  {
    m_delay = m_config.input_delay;
  }
  m_stats.delay = m_delay;
  m_stats.delay_fixed = m_fixed_delay.has_value();
  if (m_delay != from)
    NoteDelay(from, m_fixed_delay ? "chosen by the player" : "adaptive again, from the link's");
}

double Session::FramesFromMs(double ms) const
{
  return ms * m_config.frame_rate / 1000.0;
}

int Session::RoundTripFrames(int seat) const
{
  // From this machine to `seat` and back, through the relay: our typical round trip to it plus the
  // player's (one of them twice while only one is known; six frames while neither is).
  const std::optional<int> theirs = m_peer_rtt[seat];
  if (!m_typical_rtt && !theirs)
    return 6;
  const int mine = m_typical_rtt ? *m_typical_rtt : *theirs;
  const int other = theirs ? *theirs : mine;
  return static_cast<int>(std::ceil(FramesFromMs(mine + other)));
}

std::optional<int> Session::LatencyDelay() const
{
  // Rollback hides latency up to its window; input delay only covers what the window alone would
  // stall on. Through a relay, one way is half our typical round trip to it plus half the slowest
  // player's. A sample taken d frames ahead arrives one way later and runs at the boundary after,
  // with a frame kept free for jitter (LATENCY_SLACK_FRAMES); the other side runs up to
  // max_rollback frames on a guess before it stalls. The epsilon keeps an exact multiple of the
  // frame time (150 ms is 9.000 frames) from rounding up. Until our median and every other
  // player's are in, the link is unknown (nullopt), and with no other player there is none: one
  // side's alone would take the other for as far from the relay. Two frames (input_delay) cover a
  // round trip of about 230 ms between the two players.
  if (!m_typical_rtt)
    return std::nullopt;
  std::optional<int> peer;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (!InPlay(s))
      continue;
    if (!m_peer_rtt[s])
      return std::nullopt;
    peer = std::max(peer.value_or(0), *m_peer_rtt[s]);
  }
  if (!peer)
    return std::nullopt;
  const double one_way = (*m_typical_rtt + *peer) / 2.0;
  const int frames = static_cast<int>(std::ceil(FramesFromMs(one_way) - 1e-6));
  const int wanted = frames + LATENCY_SLACK_FRAMES - m_config.max_rollback;
  return std::clamp(wanted, m_config.input_delay, m_config.max_delay);
}

void Session::AdaptDelay()
{
  // Once a "second" (frame_rate frames run); the rule is at the top of this file ("Input delay").
  m_window_start = m_frame;
  m_last_resimulated = m_guessed_frames - m_window_resimulated;
  m_window_resimulated = m_guessed_frames;
  // The others' stalls since the last window, judged as each ended on its own machine. Only from
  // a player that was waiting on this one's inputs (its lag on them reached max_rollback within
  // the last EDGE_WINDOWS): one that stalled on a third player's inputs is that player's to answer.
  // The most any one of them had, so one relay hiccup that stalls two players is one event. No
  // window holds more stalls than frames (a hostile count included).
  int stalls = 0;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s == m_config.local_seat || m_peer_stalls[s] <= m_peer_stalls_seen[s])
      continue;
    if (m_frame - m_edge_frame[s] <= EDGE_WINDOWS * m_config.frame_rate)
    {
      stalls = std::max(stalls,
                        std::min(m_peer_stalls[s] - m_peer_stalls_seen[s], m_config.frame_rate));
    }
    m_peer_stalls_seen[s] = m_peer_stalls[s];
  }
  static_assert(std::tuple_size_v<decltype(m_stall_history)> == STALL_WINDOWS);
  static_assert(std::tuple_size_v<decltype(m_reach_history)> == STALL_WINDOWS);
  static_assert(RAISE_WINDOWS <= STALL_WINDOWS);
  std::shift_left(m_stall_history.begin(), m_stall_history.end(), 1);
  m_stall_history.back() = stalls;
  std::shift_left(m_reach_history.begin(), m_reach_history.end(), 1);
  m_reach_history.back() = std::exchange(m_window_peer_reach, 0);
  // Quiet: no stall, or the only stalled window of the last STALL_WINDOWS (a spike now and then).
  const auto stalled = std::count_if(m_stall_history.begin(), m_stall_history.end(),
                                     [](int n) { return n > 0; });
  const bool quiet = stalls == 0 || stalled <= QUIET_STALLED_WINDOWS;
  m_quiet_windows = quiet ? std::min(m_quiet_windows + 1, INT_MAX / 2) : 0;
  m_windows_since_lower = std::min(m_windows_since_lower + 1, INT_MAX / 2);
  // A step down that held through its probation: the quiet run the next one needs halves back
  // toward STALL_HOLD_WINDOWS (a doubled hold is for a link that keeps undoing them).
  if (m_windows_since_lower == PROBATION_WINDOWS)
    m_hold_windows = std::max(m_hold_windows / 2, STALL_HOLD_WINDOWS);
  if (m_fixed_delay || m_config.max_delay <= m_config.input_delay)
    return;

  // What the link needs. Unknown while another player's median is still missing (one who just
  // arrived): nothing steps down for the link meanwhile, unless ours never came either.
  const int link = LatencyDelay().value_or(m_typical_rtt ? m_delay : m_config.input_delay);
  m_link_below_windows = link < m_delay ? m_link_below_windows + 1 : 0;
  // Windows of the last RAISE_WINDOWS in which the others stalled on this player's inputs.
  int stalled_windows = 0;
  for (int w = STALL_WINDOWS - RAISE_WINDOWS; w < STALL_WINDOWS; ++w)
    stalled_windows += m_stall_history[w] > 0 ? 1 : 0;
  // A frame less adds a frame to every lag: at the furthest reach of the last STALL_WINDOWS
  // without a stall, the others must still have run within max_rollback - 1 of this player's
  // inputs.
  int reach = 0;
  for (int w = 0; w < STALL_WINDOWS; ++w)
  {
    if (m_stall_history[w] == 0)
      reach = std::max(reach, m_reach_history[w]);
  }
  const bool room = reach - (m_delay - 1) < m_config.max_rollback;
  // A delay the link put there follows it back down once it has wanted less for a few windows
  // (an inflated first median: pings during a keyframe's upload or download); one stalls put there
  // waits out the quiet hold, then steps a frame per window while the others stay quiet.
  const bool step_down =
      (m_delay_from_link && m_link_below_windows >= LINK_HOLD_WINDOWS) ||
      m_quiet_windows >= m_hold_windows;
  const int from = m_delay;
  if (stalled_windows >= RAISE_STALLED_WINDOWS)
  {
    // The others keep stalling on this player's inputs: a frame more (and at least the link's).
    // The stalls behind it are spent. Soon after a step down, they say the step was wrong: this
    // undoes it, and the next try needs a longer quiet run.
    std::string history;
    for (const int n : m_stall_history)
      history += fmt::format("{}{}", history.empty() ? "" : " ", n);
    m_stall_history.fill(0);
    m_quiet_windows = 0;
    m_delay_from_link = false;
    const int raised = std::clamp(m_delay + 1, link, m_config.max_delay);
    const bool revert = m_windows_since_lower < PROBATION_WINDOWS;
    if (revert)
    {
      ++m_stats.delay_reverts;
      m_hold_windows = std::min(m_hold_windows * 2, MAX_STALL_HOLD_WINDOWS);
      m_windows_since_lower = INT_MAX / 2;
    }
    else if (raised > m_delay)
    {
      ++m_stats.delay_raises;
    }
    m_delay = raised;
    if (m_delay != from)
    {
      NoteDelay(from, fmt::format("the others stalled on this player's inputs in {} of the last {} "
                                  "s (stalls per second, oldest first: {}){}",
                                  stalled_windows, RAISE_WINDOWS, history,
                                  revert ? fmt::format("; undoes a step down, the next one waits "
                                                       "for {} quiet s",
                                                       m_hold_windows) :
                                           ""));
    }
  }
  else if (link > m_delay)
  {
    RaiseToLink();
  }
  else if (m_delay > link && step_down && room)
  {
    // Above what the link needs, and a frame of room left for everyone with a frame less: one
    // frame down.
    --m_delay;
    ++m_stats.delay_lowers;
    m_windows_since_lower = 0;
    NoteDelay(from, m_delay_from_link && m_link_below_windows >= LINK_HOLD_WINDOWS ?
                        fmt::format("the link has needed {} for {} s", link,
                                    m_link_below_windows) :
                        fmt::format("{} quiet s (no stall of the others' on this player's inputs "
                                    "but a lone one), their furthest reach {} frames",
                                    m_quiet_windows, reach));
  }
  m_stats.delay = m_delay;
}

void Session::TrackAdvantage()
{
  // Like GGPO's time sync: a player whose clock runs ahead keeps running frames the others' inputs
  // haven't reached, so it rolls back deep and stalls while they never wait for it. Each side
  // measures its lead as its frame minus the other's last reported one, and reports it. Both
  // leads include the transit between them, which is the same both ways through a relay, so half
  // their difference is how far ahead this side really is, whatever either side's round trip.
  if (!m_config.time_sync || CatchingUp())
    return;
  std::optional<double> balance;
  for (int s = 0; s < m_config.seats; ++s)
  {
    // A player still catching up is behind by design: nothing to give back to it.
    if (s == m_config.local_seat || !m_advantage[s] || !InPlay(s) || !Plugged(s, m_frame))
      continue;
    auto& smoothed = m_smoothed_advantage[s];
    smoothed = smoothed ? *smoothed + (*m_advantage[s] - *smoothed) / 8 : *m_advantage[s];
    if (m_remote_advantage[s])
    {
      const double b = (*smoothed - *m_remote_advantage[s]) / 2;
      balance = std::max(balance.value_or(b), b);
    }
  }
  if (balance)
  {
    m_balance_sum += *balance;
    ++m_balance_samples;
  }
  if (m_frame - m_sync_start < TIME_SYNC_FRAMES)
    return;
  m_sync_start = m_frame;
  if (m_balance_samples > 0)
  {
    const double average = m_balance_sum / m_balance_samples;
    m_stats.frame_advantage = average;
    m_wait_pending = average > TIME_SYNC_LEAD;
  }
  m_balance_sum = 0;
  m_balance_samples = 0;
}

Pads Session::PadsFor(int frame)
{
  // The host's log decides every seat of a logged frame.
  if (frame >= m_log_base && frame < LogEnd())
    return m_log[frame - m_log_base];
  Pads pads{};
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (!Plugged(s, frame))
      pads[s] = UNPLUGGED_PAD;
  }
  if (const auto it = m_local.find(frame);
      it != m_local.end() && Plugged(m_config.local_seat, frame))
  {
    pads[m_config.local_seat] = it->second;
  }
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s == m_config.local_seat || !Plugged(s, frame))
      continue;
    const auto& inputs = m_remote[s];
    if (const auto exact = inputs.find(frame); exact != inputs.end())
    {
      pads[s] = exact->second;
      continue;
    }
    // Prediction: the last input known before this frame.
    const auto after = inputs.upper_bound(frame);
    if (after != inputs.begin())
      pads[s] = std::prev(after)->second;
  }
  return pads;
}

bool Session::NeedsSnapshot(int frame) const
{
  // Checksum frames are saved regardless, since their hash comes from the snapshot.
  if (frame % m_config.checksum_every == 0)
    return true;
  // A rollback only ever targets a frame that ran on a guess, and such a frame is past the newest
  // confirmed one: the state at the start of a confirmed frame is never loaded.
  if (frame <= ConfirmedFrame())
    return false;
  // A predicted frame is saved once it is k frames past the newest snapshot of this history (the
  // snapshots a rollback hasn't dropped). Any frame a rollback targets ran predicted, so it was
  // either saved or had a snapshot less than k frames before it, and that snapshot is still there:
  // only a rollback to an earlier frame drops it, and that re-runs the target too. So a rollback
  // never re-runs more than k - 1 frames before the first wrong one. (Every frame number divisible
  // by k would not do: frames whose inputs were in when they ran are not saved, so a rollback right
  // after a run of them would find nothing near without an extra save past the confirmed frame.)
  const auto newer = m_saved.lower_bound(frame);
  return newer == m_saved.begin() || frame - *std::prev(newer) >= m_snapshot_every;
}

int Session::ConfirmedFrame() const
{
  int confirmed = INT_MAX;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s != m_config.local_seat)
      confirmed = std::min(confirmed, KnownThrough(s));
  }
  // No seat whose inputs are still to come: everything that ran is confirmed.
  return confirmed >= NEVER ? m_frame : confirmed;
}

void Session::ReceivePackets()
{
  for (const Packet& packet : m_transport.Receive())
  {
    const int s = packet.seat;
    if (s < 0 || s >= m_config.seats || s == m_config.local_seat || !InPlay(s))
      continue;
    // Packets come from another machine through a relay: bound everything before using it. No
    // honest peer sends more than a packet's worth of inputs, inputs for frames it can't have
    // reached, or frame numbers near overflow. A player still catching up is far behind the host
    // by design: the host's own frame and checksums may be anywhere ahead (the host never sends
    // inputs beyond what the joiner said it can take).
    const int window = MAX_FRAMES_AHEAD + m_config.max_delay;
    const int far_limit = CatchingUp() ? (1 << 30) : m_frame + window;
    if (packet.pads.size() > static_cast<size_t>(MAX_PADS_IN_PACKET) || packet.first_frame < 0 ||
        packet.first_frame > m_frame + window ||
        packet.first_frame + static_cast<int>(packet.pads.size()) > m_frame + window + 1 ||
        packet.current_frame < 0 || packet.current_frame > far_limit ||
        (packet.checksum_frame && (*packet.checksum_frame < 0 || *packet.checksum_frame > far_limit)) ||
        packet.history.size() > static_cast<size_t>(MAX_HISTORY_IN_PACKET) ||
        packet.history_first < 0 || packet.history_ack > far_limit)
    {
      Fail(fmt::format("Player {} sent an invalid packet", s + 1));
      return;
    }
    const bool from_authority = s == m_config.authority_seat;
    if (from_authority && packet.roster)
    {
      ApplyRoster(*packet.roster);
      if (!m_error.empty())
        return;
    }
    if (from_authority && packet.history_seat == m_config.local_seat && !packet.history.empty())
    {
      ReceiveHistory(packet);
      if (!m_error.empty())
        return;
    }
    if (m_config.authority_seat == m_config.local_seat)
    {
      if (packet.history_ack > m_history_acked[s])
      {
        m_history_acked[s] = std::min(packet.history_ack, LogEnd() - 1);
        m_history_stuck[s] = 0;
      }
      // A player asks to leave: its controller unplugs a little ahead, so every machine hears of it
      // first. One still catching up just goes.
      if (packet.leaving)
      {
        SeatPlan& plan = m_plan[s];
        if (plan.plug_from == NEVER)
        {
          plan.live_from = NEVER;
          continue;
        }
        if (plan.unplug_from == NEVER)
        {
          plan.unplug_from = std::max(m_frame, plan.plug_from) + m_config.max_rollback +
                             2 * m_config.max_delay + 4;
        }
      }
    }
    for (size_t i = 0; i < packet.pads.size(); ++i)
    {
      const int f = packet.first_frame + static_cast<int>(i);
      const Pad& pad = packet.pads[i];
      // Inputs for frames this seat isn't plugged in at mean nothing (they never run).
      if (!Plugged(s, f) || f < LogEnd())
        continue;
      // New input for a frame that already ran and can no longer be checked against what it ran
      // with: the stall rule should make this impossible, so fail loudly rather than drift.
      if (f > m_received[s] && f < m_frame && !m_used.contains(f) && !m_remote[s].contains(f))
      {
        Fail(fmt::format("Player {}'s input for frame {} arrived after that frame could be re-run",
                         s + 1, f));
        return;
      }
      auto [it, inserted] = m_remote[s].emplace(f, pad);
      if (!inserted)
      {
        if (it->second != pad)
        {
          Fail(fmt::format("Player {} changed its input for frame {}", s + 1, f));
          return;
        }
        continue;
      }
      // A frame that already ran with a different guess must be re-run.
      if (const auto used = m_used.find(f); used != m_used.end() && used->second[s] != pad)
        m_dirty = std::min(m_dirty, f);
    }
    AdvanceReceived(s);
    const int ack = packet.ack[m_config.local_seat];
    if (ack >= -1 && ack <= m_frame + window)
      m_acked[s] = std::max(m_acked[s], ack);
    // How far this player ran past the newest of our inputs it had, while it needed them: as deep
    // as a change of them would roll it back. AdaptDelay's lowers watch it, and its stalls count
    // for our delay only when it reached max_rollback. Bounded. Not around a hitch on either
    // machine: a frozen emulator, not the link (as for stalls).
    if (ack >= 0 && Plugged(m_config.local_seat, packet.current_frame) &&
        Plugged(s, packet.current_frame) && m_last_hitch_frame < m_frame - HITCH_FRAMES)
    {
      const int lag = std::clamp(packet.current_frame - ack, 0, MAX_FRAMES_AHEAD);
      // Plus the delay: how far its frame ran ahead of this player's sampling, whatever the delay
      // (a lag clamped at 0 makes it an upper bound; for about a round trip after a change of the
      // delay, the acknowledgement is of inputs sampled at the old one).
      m_window_peer_reach = std::max(m_window_peer_reach, lag + m_delay);
      if (lag >= m_config.max_rollback)
        m_edge_frame[s] = m_frame;
    }
    m_remote_frame[s] = packet.current_frame;
    // Our lead over this peer, transit included (it cancels against the peer's own number).
    m_advantage[s] = m_frame - packet.current_frame;
    // Its own lead and round trip: telemetry, bounded rather than trusted.
    if (packet.advantage)
    {
      constexpr int bound = MAX_FRAMES_AHEAD * 10;
      m_remote_advantage[s] = std::clamp(*packet.advantage, -bound, bound) / 10.0;
    }
    if (packet.rtt && m_peer_rtt[s] != std::clamp(*packet.rtt, 0, MAX_RTT_MS))
    {
      m_peer_rtt[s] = std::clamp(*packet.rtt, 0, MAX_RTT_MS);
      m_stats.peer_rtt_median = -1;
      for (int other = 0; other < m_config.seats; ++other)
      {
        if (m_peer_rtt[other] && InPlay(other))
          m_stats.peer_rtt_median = std::max(m_stats.peer_rtt_median, *m_peer_rtt[other]);
      }
      RaiseToLink();
    }
    // Running counts: they only ever grow (a lower number is a stale or hostile one). A new stall
    // of theirs ended just now: one here around it came of it (see OnBoundary).
    if (packet.stalls > m_peer_stalls[s])
    {
      m_peer_stalls[s] = packet.stalls;
      m_last_peer_stall_frame = m_frame;
    }
    if (packet.hitches > m_peer_hitches[s])
    {
      m_peer_hitches[s] = packet.hitches;
      m_stats.peer_hitches = std::max(m_stats.peer_hitches, packet.hitches);
      m_last_hitch_frame = m_frame;
    }
    if (packet.checksum_frame)
      m_peer_checksums[*packet.checksum_frame] = packet.checksum;
  }
}

void Session::CheckChecksums()
{
  // The state at the start of frame f is final once every input for frames before f is confirmed
  // and no frame before f is waiting to be re-run.
  const int final_up_to = std::min({m_frame, ConfirmedFrame() + 1, m_dirty});
  int f = m_last_checksum_frame + m_config.checksum_every;
  while (f <= final_up_to)
  {
    const std::optional<u64> hash = m_game.SnapshotChecksum(f);
    if (!hash)
      break;  // not saved yet
    m_local_checksums[f] = *hash;
    m_pending_checksum = f;
    m_last_checksum_frame = f;
    f += m_config.checksum_every;
  }
  for (auto it = m_peer_checksums.begin(); it != m_peer_checksums.end();)
  {
    const auto mine = m_local_checksums.find(it->first);
    if (mine == m_local_checksums.end())
    {
      ++it;
      continue;
    }
    if (mine->second != it->second)
    {
      Fail(fmt::format("Desync at frame {}: {:016x} here, {:016x} there", it->first, mine->second,
                       it->second));
      return;
    }
    ++m_stats.checksums_matched;
    it = m_peer_checksums.erase(it);
  }
}

void Session::AddHistory(Packet* packet)
{
  // The host sends one joining player at a time the log frames it lacks, from where the last send
  // stopped (the relay is ordered and reliable), again from its acknowledgement if that stalls.
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (!InPlay(s))
      continue;
    const int until = std::min(m_plan[s].live_from - 1, LogEnd() - 1);
    if (m_history_acked[s] >= until)
      continue;
    if (m_history_sent[s] > m_history_acked[s] && ++m_history_stuck[s] > HISTORY_RESEND_PACKETS)
    {
      m_history_sent[s] = m_history_acked[s];
      m_history_stuck[s] = 0;
    }
    const int first = std::max(m_history_sent[s], m_history_acked[s]) + 1;
    const int last = std::min({until, first + MAX_HISTORY_IN_PACKET / 2 - 1,
                               std::max(m_remote_frame[s], m_log_base) + HISTORY_AHEAD});
    if (last < first)
      continue;
    packet->history_seat = s;
    packet->history_first = first;
    int runs = 0;
    for (int f = first; f <= last; ++f)
    {
      const Pads& pads = m_log[f - m_log_base];
      if (packet->history.empty() || packet->history.back() != pads)
      {
        if (++runs > MAX_HISTORY_RUNS)
          break;
      }
      packet->history.push_back(pads);
    }
    m_history_sent[s] = first + static_cast<int>(packet->history.size()) - 1;
    return;
  }
}

void Session::SendPacket()
{
  const bool drop_in = m_config.authority_seat >= 0;
  if (m_local.empty() && !drop_in)
    return;
  int oldest_unacked = INT_MAX;
  // The furthest frame every player still waiting for inputs can take (one catching up says how
  // far it got; MAX_FRAMES_AHEAD past that, its bounds check would refuse the packet).
  int take_up_to = INT_MAX;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (!InPlay(s))
      continue;
    oldest_unacked = std::min(oldest_unacked, m_acked[s] + 1);
    if (drop_in)
      take_up_to = std::min(take_up_to, m_remote_frame[s] + MAX_FRAMES_AHEAD - 16);
  }
  const int newest = m_local.empty() ? -1 : m_local.rbegin()->first;
  int first = m_local.empty() ? 0 :
                                std::max({oldest_unacked, newest - m_config.max_pads_per_packet + 1,
                                          m_local.begin()->first});
  int last = newest;
  // A player catching up needs the oldest inputs first, as many as fit.
  if (drop_in && !m_local.empty() && oldest_unacked != INT_MAX &&
      oldest_unacked >= m_local.begin()->first &&
      newest - oldest_unacked + 1 > m_config.max_pads_per_packet)
  {
    first = oldest_unacked;
    last = std::min(newest, first + MAX_PADS_IN_PACKET - 1);
  }
  last = std::min(last, take_up_to);
  // Nothing new since the last packet and no checksum to report: stay quiet (a drop-in session
  // always reports, since its frame, acknowledgements and roster are what a joining player paces
  // on).
  if (!drop_in && newest == m_last_sent_newest && !m_pending_checksum && first > newest)
    return;

  Packet packet;
  packet.seat = m_config.local_seat;
  packet.first_frame = first <= last ? first : 0;
  for (int f = first; f <= last; ++f)
  {
    const auto it = m_local.find(f);
    packet.pads.push_back(it != m_local.end() ? it->second : Pad{});
  }
  if (drop_in)
  {
    packet.history_ack = LogEnd() - 1;
    packet.leaving = m_leaving;
    if (m_config.authority_seat == m_config.local_seat)
    {
      packet.roster = m_plan;
      AddHistory(&packet);
    }
  }
  for (int s = 0; s < m_config.seats; ++s)
    packet.ack[s] = s == m_config.local_seat ? -1 : m_received[s];
  packet.current_frame = m_frame;
  packet.resimulated = m_last_resimulated;
  packet.stalls = m_link_stalls;
  packet.hitches = m_hitches;
  std::optional<double> lead;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s != m_config.local_seat && m_smoothed_advantage[s])
      lead = std::max(lead.value_or(*m_smoothed_advantage[s]), *m_smoothed_advantage[s]);
  }
  if (lead)
  {
    constexpr double bound = MAX_FRAMES_AHEAD;
    packet.advantage = static_cast<int>(std::lround(std::clamp(*lead, -bound, bound) * 10));
  }
  packet.rtt = m_typical_rtt;
  if (m_pending_checksum)
  {
    packet.checksum_frame = m_pending_checksum;
    packet.checksum = m_local_checksums[*m_pending_checksum];
    m_pending_checksum.reset();
  }
  m_last_sent_newest = newest;
  m_transport.Send(packet);
}
}  // namespace Orca::Net
