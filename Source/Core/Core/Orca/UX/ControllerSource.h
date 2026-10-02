// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

// The local player's controller from the YouGame desktop app: the app's controller helper reads
// GameCube adapters (libusb, raw bytes) and SDL gamepads, and serves them to the running native
// game as an authenticated SSE stream, GET $YOUGAME_BRIDGE/controllers/events. One thread keeps
// that stream open and the newest snapshot in a one-slot cache; the CPU thread reads the cache once
// per frame boundary and never waits on the network.
//
// Which pad (Controllers.h ChooseLocalPad): the adapter port the player claimed in YouGame's
// Controls panel (seat 0), else the lowest-seat port with a controller, else the first standard
// gamepad, else none (Dolphin's GCPadNew.ini mapping, as before). Stale (no report for 250 ms) or
// suspended input is neutral. The session records what this returns and sends it, so rollback
// replays the recorded pad, never a fresh read.
namespace Orca::UX
{
// The pad for this frame boundary, or nullopt to use Pad::GetStatus(0). CPU thread; never starts
// anything. Follows Dolphin's input gate (focus) like the keyboard does: unfocused is neutral.
std::optional<GCPadStatus> LocalPad();

// The stream under the app (YOUGAME_BRIDGE and YOUGAME_TOKEN on loopback); nothing otherwise.
// From UX::Init, on the main thread, before the game boots.
void StartControllerStreamFromEnvironment();

// The stream for an explicit bridge (tests; LocalPad() uses the environment).
void StartControllerStream(const std::string& bridge, const std::string& token);
void StopControllerStream();
// The newest snapshot's pad without the focus gate (tests and tools).
std::optional<GCPadStatus> LatestPad();

// Transport timing, for the latency check: per snapshot that carried a new adapter report, the
// time from the helper receiving the USB report to this process having it parsed in the cache
// (both epoch ms; the helper anchors its clock to the wall clock once at start).
struct ControllerTiming
{
  u64 snapshots = 0;
  u64 refused = 0;  // malformed events
  u64 samples = 0;
  double p50_ms = 0, p95_ms = 0, p99_ms = 0, max_ms = 0;
  bool attached = false;
};
ControllerTiming GetControllerTiming();
void ResetControllerTiming();
}  // namespace Orca::UX
