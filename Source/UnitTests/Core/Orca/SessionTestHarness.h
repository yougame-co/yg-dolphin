// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "Core/Orca/Session/Session.h"

namespace Orca::Net::Test
{
// A deterministic "game": its state is a hash of every frame number and every pad it ran with.
class FakeGame final : public Game
{
  static u64 Mix(u64 h, u64 v)
  {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
  }

public:
  bool Save(int frame) override
  {
    // Saving the same frame twice between loads is wasted work (a full snapshot in Dolphin).
    if (!m_saved_since_load.insert(frame).second)
      ++m_double_saves;
    ++m_saves;
    // Like SnapshotRing: a new frame takes the next slot, evicting what it held.
    if (ring_slots > 0 && !m_snapshots.contains(frame))
    {
      m_ring.resize(ring_slots, -1);
      m_snapshots.erase(m_ring[m_next]);
      m_ring[m_next] = frame;
      m_next = (m_next + 1) % ring_slots;
    }
    m_snapshots[frame] = m_state;
    return true;
  }
  bool Load(int frame) override
  {
    m_saved_since_load.clear();
    const auto it = m_snapshots.find(frame);
    if (it == m_snapshots.end())
      return false;
    m_state = it->second;
    m_loads.emplace_back(frame, m_state);
    // Like SnapshotRing: the snapshots after the loaded frame are gone (the re-run saves them
    // again), and the next save takes the slot after the loaded one.
    m_snapshots.erase(m_snapshots.upper_bound(frame), m_snapshots.end());
    if (ring_slots > 0)
    {
      const auto slot = std::find(m_ring.begin(), m_ring.end(), frame);
      for (int& f : m_ring)
      {
        if (f > frame)
          f = -1;
      }
      m_next = static_cast<size_t>(slot - m_ring.begin() + 1) % ring_slots;
    }
    return true;
  }
  void SetPads(int frame, const Pads& pads) override
  {
    m_history[frame] = m_state;  // the state at the start of `frame` (re-runs overwrite)
    m_ran_pads[frame] = pads;
    m_pads_frame = frame;
    m_pads = pads;
  }
  void SetResimulating(bool resimulating) override { m_resimulating = resimulating; }
  std::optional<u64> SnapshotChecksum(int frame) override
  {
    const auto it = m_snapshots.find(frame);
    if (it == m_snapshots.end())
      return std::nullopt;
    return it->second;
  }
  void ResetPacing() override { ++m_pacing_resets; }
  double SaveMs() const override { return save_ms; }
  double NowMs() const override { return now_ms; }

  // Runs the frame whose pads were set last; returns its number.
  int Run()
  {
    u64 h = Mix(m_state, static_cast<u64>(m_pads_frame));
    for (const Pad& pad : m_pads)
      for (u8 b : pad)
        h = Mix(h, b);
    m_state = h;
    // A bug that corrupts this frame on every run, re-runs included.
    if (m_corrupt_at == m_pads_frame)
      m_state ^= 1;
    return m_pads_frame;
  }

  // Slots like RingPort's ring (0: keep every snapshot), what a save reports it costs, and the
  // simulated clock (Game::NowMs).
  size_t ring_slots = 0;
  double save_ms = 0;
  double now_ms = 0;
  std::vector<int> m_ring;
  size_t m_next = 0;
  std::map<int, u64> m_snapshots;
  std::set<int> m_saved_since_load;
  int m_saves = 0;
  int m_double_saves = 0;
  std::vector<std::pair<int, u64>> m_loads;
  std::map<int, u64> m_history;
  std::map<int, Pads> m_ran_pads;
  u64 m_state = 1;
  int m_pads_frame = -1;
  Pads m_pads{};
  bool m_resimulating = false;
  int m_pacing_resets = 0;
  std::optional<int> m_corrupt_at;
};

}  // namespace Orca::Net::Test
