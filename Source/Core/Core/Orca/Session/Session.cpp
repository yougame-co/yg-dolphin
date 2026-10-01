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
// Quiet windows in a row before the delay steps down; doubles each time a lower has to be undone.
constexpr int QUIET_WINDOWS = 10;
constexpr int MAX_QUIET_WINDOWS = 1024;
// Round trips may raise the delay during the first seconds of play only: later, a raise from a
// single slow ping would cost responsiveness the rollbacks never asked for. After that they only
// keep quiet windows from lowering it below what the link needs, once this many have come in: a
// lone stray ping can't hold the delay up all match. (A bad first one still takes about twenty
// samples to fade from the smoothed estimate; until then it only delays lowers.)
constexpr int START_SECONDS = 3;
constexpr int FLOOR_SAMPLES = 8;
// Stalls drive the delay only on a rate: STALLS_TO_RAISE counted stalls (here and reported) in the
// last STALL_WINDOWS windows. A one-off stall is as often a hitch the frame timer missed as the
// link, and one frame of delay can't cover a hitch; a link too slow for the delay stalls again and
// again. A delay raised for stalls then holds STALL_HOLD_WINDOWS stall-free windows before the
// quiet-window rule may lower it again.
constexpr int STALL_WINDOWS = 3;
constexpr int STALLS_TO_RAISE = 2;
constexpr int STALL_HOLD_WINDOWS = 8;
// The first seconds of a session boot the game and load menus from the disc, and stall now and
// then on the emulator rather than the link: until WARMUP_SECONDS, a raise takes stalls several
// times a second (WARMUP_STALLS_TO_RAISE in STALL_WINDOWS windows), which a link far too slow for
// the delay still makes.
constexpr int WARMUP_SECONDS = 10;
constexpr int WARMUP_STALLS_TO_RAISE = 3;
// A stall that began within this many frames after a hitch on any machine (or during which one was
// heard of) is the hitch's, not the link's.
constexpr int HITCH_FRAMES = 30;
// No working relay is ten seconds away.
constexpr int MAX_RTT_MS = 10000;
// Time sync averages this many frames, waits at most once per that many, and only when it is more
// than this many frames ahead on average: anything less is jitter.
constexpr int TIME_SYNC_FRAMES = 30;
constexpr double TIME_SYNC_LEAD = 1.0;
}  // namespace

Session::Session(const Config& config, Game& game, Transport& transport)
    : m_config(config), m_game(game), m_transport(transport), m_dirty(INT_MAX),
      m_delay(config.input_delay), m_quiet_needed(QUIET_WINDOWS)
{
  m_config.max_delay = std::max(m_config.max_delay, m_config.input_delay);
  m_config.frame_rate = std::max(m_config.frame_rate, 1);
  m_stats.delay = m_delay;
  // Frames before the input delay have no sampled input: both sides treat them as neutral, and
  // sending them explicitly lets the other side confirm them like any other frame.
  for (int f = 0; f < m_config.input_delay; ++f)
    m_local[f] = Pad{};
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
  if (!m_transport.Connected())
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
  SendPacket();

  // A frame that already ran with a wrong guess: rewind to it and re-run up to the newest frame.
  if (m_dirty < m_frame)
  {
    const int from = m_dirty;
    m_dirty = INT_MAX;
    const int depth = m_frame - from;
    if (depth > m_config.max_rollback)
      return Fail(fmt::format("Rollback of {} frames exceeds the limit", depth));
    m_last_saved = -1;
    if (!m_game.Load(from))
      return Fail(fmt::format("Loading frame {} failed", from));
    ++m_stats.rollbacks;
    m_stats.resimulated_frames += depth;
    m_stats.deepest_rollback = std::max(m_stats.deepest_rollback, depth);
    m_resim_until = m_frame - 1;
    m_game.SetResimulating(true);
    const Pads pads = PadsFor(from);
    m_used[from] = pads;
    m_game.SetPads(from, pads);
    return {StepKind::Rollback, from};
  }

  // Too far ahead of the slowest player's inputs: wait without running anything.
  if (m_frame - ConfirmedFrame() > m_config.max_rollback)
  {
    if (!m_stalling)
    {
      ++m_stats.stalls;
      m_stall_frame = m_frame;
      m_stall_after_input = ConfirmedFrame() >= 0;
    }
    m_stalling = true;
    return {StepKind::Stall, m_frame};
  }
  // A stall just ended: judged now, when the packet that ended it (and a hitch it reports) is in.
  // Waiting for a peer's first packet is the trip, not the delay.
  if (m_stalling && m_stall_after_input && m_last_hitch_frame < m_stall_frame - HITCH_FRAMES)
    ++m_link_stalls;
  // After a stall or a wait, the time it held is not made up.
  if (m_stalling || m_waiting)
    m_game.ResetPacing();
  m_stalling = false;
  m_waiting = false;

  const int run = m_frame;
  // Every frame runs with this player's own input for it, the one the others receive.
  if (!m_local.contains(run))
    return Fail(fmt::format("No local input for frame {}", run));
  const Pads pads = PadsFor(run);
  m_used[run] = pads;
  m_game.SetPads(run, pads);
  ++m_frame;
  if (m_frame - m_window_start >= m_config.frame_rate)
    AdaptDelay();
  TrackAdvantage();

  // History older than the deepest possible rollback is no longer needed. Keep one remote input
  // before the window, since predictions repeat the last known input.
  const int oldest = m_frame - m_config.max_rollback - 2;
  int oldest_unacked = INT_MAX;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s != m_config.local_seat)
      oldest_unacked = std::min(oldest_unacked, m_acked[s] + 1);
  }
  const int keep_local = std::min(oldest, oldest_unacked);
  std::erase_if(m_local, [keep_local](const auto& e) { return e.first < keep_local; });
  std::erase_if(m_used, [oldest](const auto& e) { return e.first < oldest; });
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
  return true;
}

void Session::SampleLocal(const Pad& local_pad)
{
  const int target = m_frame + m_delay;
  const int newest = m_local.empty() ? -1 : m_local.rbegin()->first;
  // After a lower, the target already has an input (it went out with an earlier packet): this
  // sample is dropped, and the next boundary's lands on the frame after. A stalled or waiting
  // boundary repeats its frame and lands here too.
  if (target <= newest)
    return;
  // After a raise, the frames skipped over repeat the last input (a held direction stays held) and
  // travel like any other, so the other players can confirm them.
  const Pad held = newest >= 0 ? m_local.rbegin()->second : Pad{};
  for (int f = newest + 1; f < target; ++f)
    m_local[f] = held;
  m_local[target] = local_pad;
}

void Session::OnRoundTrip(int rtt_ms)
{
  // Nothing a working relay measures; a broken clock or a ping answered after a freeze.
  if (rtt_ms < 0 || rtt_ms > MAX_RTT_MS)
    return;
  // Smoothed like TCP's estimator: an eighth of each error moves the mean, a quarter the deviation.
  if (!m_rtt_ms)
  {
    m_rtt_ms = rtt_ms;
  }
  else
  {
    const double error = rtt_ms - *m_rtt_ms;
    *m_rtt_ms += error / 8;
    m_rtt_dev_ms += (std::abs(error) - m_rtt_dev_ms) / 4;
  }
  ++m_rtt_samples;
  ApplyStartDelay();
}

void Session::OnLocalHitch()
{
  ++m_hitches;
  m_last_hitch_frame = m_frame;
}

void Session::ApplyStartDelay()
{
  // In the first seconds the delay covers what the rollback window can't (LatencyDelay), and
  // follows the estimate as round trips (ours, then the peer's) come in. Down too, but only while
  // nothing else moved it: a rise the rollbacks asked for stays.
  if (!m_rtt_ms || m_frame >= START_SECONDS * m_config.frame_rate)
    return;
  const int wanted = LatencyDelay();
  const bool ours = m_start_delay == 0 ? m_delay == m_config.input_delay : m_delay == m_start_delay;
  if (wanted > m_delay || (wanted < m_delay && ours))
  {
    m_delay = wanted;
    m_start_delay = wanted;
    m_stats.delay = m_delay;
  }
}

double Session::FramesFromMs(double ms) const
{
  return ms * m_config.frame_rate / 1000.0;
}

int Session::LatencyDelay() const
{
  // Rollback hides latency up to its window; input delay only covers what the window alone would
  // stall on. Through a relay, one way is half our round trip to it plus half the peer's (the
  // slowest peer's that reported one; ours twice until then). The deviation covers packets later
  // than average, and a rollback this deep (max_rollback - 2) still leaves room for jitter. The
  // epsilon keeps an exact multiple of the frame time (150 ms is 9.000 frames) from rounding up.
  std::optional<int> peer;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s != m_config.local_seat && m_peer_rtt[s])
      peer = std::max(peer.value_or(*m_peer_rtt[s]), *m_peer_rtt[s]);
  }
  const double one_way = (*m_rtt_ms + peer.value_or(*m_rtt_ms)) / 2 + m_rtt_dev_ms;
  const int frames = static_cast<int>(std::ceil(FramesFromMs(one_way) - 1e-6));
  const int wanted = frames - (m_config.max_rollback - 2);
  return std::clamp(wanted, m_config.input_delay, m_config.max_delay);
}

int Session::DelayFloor() const
{
  // After the start, the link estimate still bounds how far quiet windows may lower the delay.
  if (!m_rtt_ms || m_rtt_samples < FLOOR_SAMPLES)
    return m_config.input_delay;
  return LatencyDelay();
}

void Session::AdaptDelay()
{
  // A delay shorter than the link makes every wrong guess re-simulate several frames inside one
  // displayed frame; a machine that spends its budget re-simulating answers late, the others guess
  // deeper, and the match spirals. So once a "second" (frame_rate frames run): re-simulating half
  // a frame per frame on average, or any stall, raises the delay one frame; enough quiet windows
  // in a row lower it one frame towards input_delay. A lower that brings the trouble back is
  // undone at the next window, and the quiet needed before the next try doubles.
  //
  // One side's stalls come from the other side's delay (its inputs arrive too late for this side's
  // window), so stalls count from both sides, like re-simulated frames. They raise on a rate
  // (STALLS_TO_RAISE in STALL_WINDOWS windows, more during the warmup), not one at a time, and
  // stalls lined up with a hitch on either machine, which no delay cures, don't count at all.
  const int mine = m_stats.resimulated_frames - m_window_resimulated;
  // The window's stalls are the most any one side had: one relay hiccup that stalls both sides
  // once is one event, not two.
  int stalls = m_link_stalls - m_window_link_stalls;
  m_window_start = m_frame;
  m_window_resimulated = m_stats.resimulated_frames;
  m_window_link_stalls = m_link_stalls;
  m_last_resimulated = mine;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s == m_config.local_seat || m_peer_stalls[s] <= m_peer_stalls_seen[s])
      continue;
    // No window holds more stalls than frames (a hostile count included).
    stalls = std::max(stalls,
                      std::min(m_peer_stalls[s] - m_peer_stalls_seen[s], m_config.frame_rate));
    m_peer_stalls_seen[s] = m_peer_stalls[s];
  }
  static_assert(std::tuple_size_v<decltype(m_stall_history)> == STALL_WINDOWS);
  std::shift_left(m_stall_history.begin(), m_stall_history.end(), 1);
  m_stall_history.back() = stalls;
  int recent = 0;
  for (const int n : m_stall_history)
    recent += n;
  if (m_config.max_delay <= m_config.input_delay)
    return;

  // Our rollbacks come from the others' delay and theirs from ours: adapt on the worst side.
  int resimulated = mine;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s != m_config.local_seat)
      resimulated = std::max(resimulated, m_peer_resimulated[s]);
  }
  const int rate = m_config.frame_rate;
  const bool quiet = resimulated * 20 <= rate && stalls == 0;
  const bool warm = m_frame >= WARMUP_SECONDS * rate;
  const bool stalling = recent >= (warm ? STALLS_TO_RAISE : WARMUP_STALLS_TO_RAISE);
  if (stalls == 0 && m_stall_hold > 0)
    --m_stall_hold;

  // On probation after a lower: trouble means the lower was wrong, straight back up. Two windows,
  // where the SDK watches one: a lower makes the other side roll back, and its report of that
  // arrives a window late.
  if (m_tried_delay > 0)
  {
    if (!quiet)
    {
      m_quiet_windows = 0;
      m_quiet_needed = std::min(m_quiet_needed * 2, MAX_QUIET_WINDOWS);
      if (m_delay < m_tried_delay)
      {
        m_delay = m_tried_delay;
        ++m_stats.delay_reverts;
      }
      m_tried_delay = 0;
      m_stats.delay = m_delay;
      // Stalls that undid a lower don't raise again on top.
      if (stalling)
        m_stall_history.fill(0);
      return;
    }
    if (--m_probe_windows == 0)
      m_tried_delay = 0;
  }

  if (resimulated * 2 >= rate || stalling)
  {
    m_quiet_windows = 0;
    if (m_delay < m_config.max_delay)
    {
      ++m_delay;
      ++m_stats.delay_raises;
    }
    // The stalls behind a raise are spent: the next one needs a new run of them.
    if (stalling)
    {
      m_stall_history.fill(0);
      m_stall_hold = STALL_HOLD_WINDOWS;
    }
  }
  else if (quiet)
  {
    // A steady link (round trips within a frame of each other) earns a lower in half the time.
    int needed = m_quiet_needed;
    if (m_rtt_ms && FramesFromMs(m_rtt_dev_ms) <= 1.0)
      needed = std::max(5, needed / 2);
    if (++m_quiet_windows >= needed && m_stall_hold == 0 && m_delay > DelayFloor())
    {
      m_quiet_windows = 0;
      m_tried_delay = m_delay;
      m_probe_windows = 2;
      --m_delay;
      ++m_stats.delay_lowers;
    }
  }
  else
  {
    m_quiet_windows = 0;
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
  if (!m_config.time_sync)
    return;
  std::optional<double> balance;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s == m_config.local_seat || !m_advantage[s])
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
  Pads pads{};
  if (const auto it = m_local.find(frame); it != m_local.end())
    pads[m_config.local_seat] = it->second;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s == m_config.local_seat)
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
  // A rollback only ever targets a frame that ran on a guess, and such a frame is past the newest
  // confirmed one: the state at the start of a confirmed frame is never loaded. Checksum frames are
  // saved regardless, since their hash comes from the snapshot.
  return frame > ConfirmedFrame() || frame % m_config.checksum_every == 0;
}

int Session::ConfirmedFrame() const
{
  int confirmed = INT_MAX;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s != m_config.local_seat)
      confirmed = std::min(confirmed, m_received[s]);
  }
  return confirmed == INT_MAX ? m_frame : confirmed;
}

void Session::ReceivePackets()
{
  for (const Packet& packet : m_transport.Receive())
  {
    const int s = packet.seat;
    if (s < 0 || s >= m_config.seats || s == m_config.local_seat)
      continue;
    // Packets come from another machine through a relay: bound everything before using it. No
    // honest peer sends more than a packet's worth of inputs, inputs for frames it can't have
    // reached, or frame numbers near overflow.
    const int window = MAX_FRAMES_AHEAD + m_config.max_delay;
    if (packet.pads.size() > static_cast<size_t>(MAX_PADS_IN_PACKET) || packet.first_frame < 0 ||
        packet.first_frame > m_frame + window ||
        packet.first_frame + static_cast<int>(packet.pads.size()) > m_frame + window + 1 ||
        packet.current_frame < 0 || packet.current_frame > m_frame + window ||
        (packet.checksum_frame &&
         (*packet.checksum_frame < 0 || *packet.checksum_frame > m_frame + window)))
    {
      Fail(fmt::format("Player {} sent an invalid packet", s + 1));
      return;
    }
    for (size_t i = 0; i < packet.pads.size(); ++i)
    {
      const int f = packet.first_frame + static_cast<int>(i);
      const Pad& pad = packet.pads[i];
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
    while (m_remote[s].contains(m_received[s] + 1))
      ++m_received[s];
    const int ack = packet.ack[m_config.local_seat];
    if (ack >= -1 && ack <= m_frame + window)
      m_acked[s] = std::max(m_acked[s], ack);
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
      ApplyStartDelay();
    }
    // No window re-simulates more than every frame of the deepest rollback.
    if (packet.resimulated >= 0)
    {
      m_peer_resimulated[s] =
          std::min(packet.resimulated, m_config.frame_rate * m_config.max_rollback);
    }
    // Running counts: they only ever grow (a lower number is a stale or hostile one).
    m_peer_stalls[s] = std::max(m_peer_stalls[s], packet.stalls);
    if (packet.hitches > m_peer_hitches[s])
    {
      m_peer_hitches[s] = packet.hitches;
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

void Session::SendPacket()
{
  if (m_local.empty())
    return;
  const int newest = m_local.rbegin()->first;
  int oldest_unacked = INT_MAX;
  for (int s = 0; s < m_config.seats; ++s)
  {
    if (s != m_config.local_seat)
      oldest_unacked = std::min(oldest_unacked, m_acked[s] + 1);
  }
  const int first = std::max({oldest_unacked, newest - m_config.max_pads_per_packet + 1,
                              m_local.begin()->first});
  // Nothing new since the last packet and no checksum to report: stay quiet.
  if (newest == m_last_sent_newest && !m_pending_checksum && first > newest)
    return;

  Packet packet;
  packet.seat = m_config.local_seat;
  packet.first_frame = first;
  for (int f = first; f <= newest; ++f)
  {
    const auto it = m_local.find(f);
    packet.pads.push_back(it != m_local.end() ? it->second : Pad{});
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
  if (m_rtt_ms)
    packet.rtt = static_cast<int>(std::lround(*m_rtt_ms));
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
