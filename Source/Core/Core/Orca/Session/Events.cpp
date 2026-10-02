// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/Events.h"

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
