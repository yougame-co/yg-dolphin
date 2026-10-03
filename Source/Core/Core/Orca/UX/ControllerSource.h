// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
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
// `for_frame`: a frame's own read (Events::LocalPadSource), timed for the input age.
std::optional<GCPadStatus> LocalPad(bool for_frame = false);

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
// (both epoch ms; the helper anchors its clock to the wall clock once at start, so this carries the
// two clocks' offset and drift: fine for that check's short runs, not for a long session).
//
// Input age at the frame boundary (ORCA.md, "Controller input age"), from each frame's own read of
// an adapter port (LocalPad(true)): how old the newest adapter report was at that read. Sampled
// only at reads whose pad differs from the previous read's (a change): an unchanged pad's age says
// nothing, and grows wherever reports pause. Made of two spans, each timed on one clock: the
// helper's (sent_at - received_at, from the USB report to the snapshot leaving the helper, or the
// app for a snapshot it held) and how long the snapshot then sat in this process's cache. With
// every report published, that is about half the report interval, so faster USB polling shows
// here. Not counted: the hop through the app into this process (its two ends are different
// processes' wall clocks, whose offset and drift would land in it in full; its jitter, above the
// fastest hop of the last second, is reported beside the age as "hop"), the adapter's own
// controller poll (a report's data is up to ~1.2 ms old when it leaves the adapter) and the host's
// USB stack.
struct ControllerTiming
{
  u64 snapshots = 0;
  u64 refused = 0;  // malformed events
  u64 samples = 0;
  double p50_ms = 0, p95_ms = 0, p99_ms = 0, max_ms = 0;
  bool attached = false;
  // Input age: totals since the stream started (ResetControllerTiming clears them), and the
  // percentiles of the newest 4096 changes.
  Orca::Events::InputAge input;
  u64 age_samples = 0;
  double age_p50_ms = 0, age_p95_ms = 0, age_p99_ms = 0, age_max_ms = 0;
  double helper_p50_ms = 0, cache_p50_ms = 0;
  // The hops of the snapshots those changes read, above the fastest of the second before each.
  u64 hop_samples = 0;
  double hop_p50_ms = 0, hop_p95_ms = 0;
};
ControllerTiming GetControllerTiming();
void ResetControllerTiming();
// The input age totals alone (cheap: no percentiles), for Events::GetInputAge.
Orca::Events::InputAge GetInputAge();
}  // namespace Orca::UX
