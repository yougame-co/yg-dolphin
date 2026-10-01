// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/Session.h"

#include <algorithm>
#include <climits>

#include <fmt/format.h>

namespace Orca::Net
{
Session::Session(const Config& config, Game& game, Transport& transport)
    : m_config(config), m_game(game), m_transport(transport), m_dirty(INT_MAX)
{
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
    if (!m_game.Save(next))
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
    if (!m_game.Save(m_frame))
      return Fail(fmt::format("Saving frame {} failed", m_frame));

    const int target = m_frame + m_config.input_delay;
    if (!m_local.contains(target))
      m_local[target] = local_pad;
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
      ++m_stats.stalls;
    m_stalling = true;
    return {StepKind::Stall, m_frame};
  }
  if (m_stalling)
    m_game.ResetPacing();
  m_stalling = false;

  const int run = m_frame;
  const Pads pads = PadsFor(run);
  m_used[run] = pads;
  m_game.SetPads(run, pads);
  ++m_frame;

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
    for (size_t i = 0; i < packet.pads.size(); ++i)
    {
      const int f = packet.first_frame + static_cast<int>(i);
      const Pad& pad = packet.pads[i];
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
    m_acked[s] = std::max(m_acked[s], packet.ack[m_config.local_seat]);
    m_remote_frame[s] = packet.current_frame;
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
