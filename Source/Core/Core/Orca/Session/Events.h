// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

// What the online session tells the rest of Orca (the in-game UX: overlay, name tags, the desktop
// app's netlog). The session owns the data; readers only get copies and callbacks.
namespace Orca::Events
{
// A controller port in play: who holds it, by their YouGame username (from the join handshake).
struct PortInfo
{
  int port = 0;  // 0-based GameCube port
  std::string name;
  bool remote = false;
};

// A friend plugged in (or the ports changed): fires on the CPU thread at the frame boundary of
// `frame` on every machine in the session, inside the rollback domain, so it runs again if that
// frame is re-simulated. Writes to emulated memory from here must depend only on `frame` and
// `ports`. One callback; setting it again replaces it, an empty function clears it.
using PlugInCallback = std::function<void(int frame, const std::vector<PortInfo>& ports)>;
void SetPlugInCallback(PlugInCallback callback);

// How the link is doing, refreshed about once a second. Any thread.
struct LinkStats
{
  bool online = false;  // a friend is plugged in
  int delay = 0;        // input delay, in frames
  double rollbacks_per_second = 0;
  int deepest_rollback = 0;  // frames, in the last second
  int stalls = 0;            // frames the game waited for a friend's input, in total
  int hitches = 0;           // this machine's slow frames, in total
  int peer_hitches = 0;
  int round_trip_ms = -1;  // to the room server; -1 before the first
  int confirmed_frame = -1;
  // The game is waiting for a friend's input right now: for how long, and whose seat (-1: nobody).
  int stalled_ms = 0;
  int waiting_seat = -1;
};
LinkStats GetLinkStats();

// The local player's controller, if something other than Dolphin's own pad input supplies it (the
// YouGame app's controller stream). Called on the CPU thread once per frame boundary, solo or in a
// session; it must not block. nullopt: use Dolphin's pad. The session records what it returns, so
// a re-run frame replays it and never asks again.
using LocalPadSource = std::function<std::optional<GCPadStatus>()>;
void SetLocalPadSource(LocalPadSource source);
std::optional<GCPadStatus> LocalPad();

// The session's side (Rollback / Online): publish the latest stats, and fire the plug-in callback.
void PublishLinkStats(const LinkStats& stats);
void NotifyPlugIn(int frame, const std::vector<PortInfo>& ports);
}  // namespace Orca::Events
