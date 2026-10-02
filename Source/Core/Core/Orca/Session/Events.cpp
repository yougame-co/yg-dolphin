// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/Events.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace Orca::Events
{
namespace
{
std::mutex s_lock;
PlugInCallback s_plug_in;
LinkStats s_stats;
LocalPadSource s_pad_source;
FrameCallback s_frame;
StallCallback s_stall;
std::atomic<int> s_ux_compat{0};
}  // namespace

void SetPlugInCallback(PlugInCallback callback)
{
  std::lock_guard lk(s_lock);
  s_plug_in = std::move(callback);
}

LinkStats GetLinkStats()
{
  std::lock_guard lk(s_lock);
  return s_stats;
}

void PublishLinkStats(const LinkStats& stats)
{
  std::lock_guard lk(s_lock);
  s_stats = stats;
}

void SetLocalPadSource(LocalPadSource source)
{
  std::lock_guard lk(s_lock);
  s_pad_source = std::move(source);
}

std::optional<GCPadStatus> LocalPad()
{
  LocalPadSource source;
  {
    std::lock_guard lk(s_lock);
    source = s_pad_source;
  }
  return source ? source() : std::nullopt;
}

void NotifyPlugIn(int frame, const std::vector<PortInfo>& ports)
{
  PlugInCallback callback;
  {
    std::lock_guard lk(s_lock);
    callback = s_plug_in;
  }
  if (callback)
    callback(frame, ports);
}
}  // namespace Orca::Events

namespace Orca::Events
{
void SetFrameCallback(FrameCallback callback)
{
  std::lock_guard lk(s_lock);
  s_frame = std::move(callback);
}

void SetStallCallback(StallCallback callback)
{
  std::lock_guard lk(s_lock);
  s_stall = std::move(callback);
}

void SetUXCompatVersion(int version)
{
  s_ux_compat.store(version, std::memory_order_relaxed);
}

int UXCompatVersion()
{
  return s_ux_compat.load(std::memory_order_relaxed);
}

void NotifyFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating)
{
  FrameCallback callback;
  {
    std::lock_guard lk(s_lock);
    callback = s_frame;
  }
  if (callback)
    callback(guard, frame, resimulating);
}

void NotifyStall(int stalled_ms)
{
  StallCallback callback;
  {
    std::lock_guard lk(s_lock);
    callback = s_stall;
  }
  if (callback)
    callback(stalled_ms);
}
}  // namespace Orca::Events
