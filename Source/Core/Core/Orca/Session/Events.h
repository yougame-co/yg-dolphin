// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
}

// What the online session tells the rest of Orca (the in-game UX: overlay, name tags, the desktop
// app's netlog). The session owns the data; readers only get copies and callbacks.
namespace Orca::Events
{
// A controller port in play: who holds it, by their YouGame username (from the join handshake).
struct PortInfo
{
  int port = 0;  // 0-based GameCube port
  std::string name;
  // Whether another machine holds this port. This differs between machines (each sees its own port
  // as local): UI only, never part of a memory write.
  bool remote = false;
  bool operator==(const PortInfo&) const = default;
};

// The plugged-in ports changed (a friend plugged in or out): on the CPU thread at the boundary of
// `frame`, first runs only, for the UI (toasts, the overlay). Not for memory writes: a re-run never
// repeats it (the frame callback's `ports` is what writes use). One callback; setting it again
// replaces it, an empty function clears it.
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
  // How the friends' inputs arrive (the worst of them): 0 the relay, 1 a direct link, 2 a direct
  // link through TURN; and that link's round trip (-1: none).
  int transport = 0;
  int link_rtt_ms = -1;
  int confirmed_frame = -1;
  // The game is waiting for a friend's input right now: for how long, and whose seat (-1: nobody).
  int stalled_ms = 0;
  int waiting_seat = -1;
};
LinkStats GetLinkStats();

// The local player's controller, if something other than Dolphin's own pad input supplies it (the
// YouGame app's controller stream). Called on the CPU thread once per frame boundary, solo or in a
// session; it must not block. nullopt: use Dolphin's pad. The session records what it returns, so
// a re-run frame replays it and never asks again. `for_frame`: this read is a frame's own input (a
// first run's boundary; not a re-run's or a catch-up's, whose reads nothing uses), so the source
// times it (InputAge).
using LocalPadSource = std::function<std::optional<GCPadStatus>(bool for_frame)>;
void SetLocalPadSource(LocalPadSource source);
std::optional<GCPadStatus> LocalPad(bool for_frame = false);

// How old the local controller's input was when frames read it (ORCA.md, "Controller input age"):
// running totals from the pad source, which counts only a device whose reports carry their arrival
// time (a GameCube adapter through the app's helper). A reader takes differences; the totals start
// again from zero when the app's stream does. Any thread.
struct InputAge
{
  u64 frames = 0;     // frames that read such a device
  u64 reports = 0;    // reports that came in from it across those frames
  u64 changes = 0;    // frames whose pad differed from the frame before's: the age samples
  double age_ms = 0;  // summed over the changes: the input age at that read (ControllerSource.h)
};
using InputAgeSource = std::function<InputAge()>;
void SetInputAgeSource(InputAgeSource source);
InputAge GetInputAge();

// Every frame boundary, on every machine, solo or online, first runs and re-runs alike: before
// anything is saved at this boundary (a rollback snapshot, a keyframe), so what it writes is in
// them, and again whenever the frame is re-run. `frame` is the frame about to run (the numbers
// NotifyPlugIn uses); `resimulating` while a rollback re-runs frames. A write to emulated memory
// from here must be a pure function of that memory and synced data, and idempotent.
// `ports`: the controllers plugged in at `frame`, from the session's synced plan and the room's
// roster names, identical on every machine for the same frame (solo: this player's port). Use these
// for memory writes, never NotifyPlugIn's (that one is for the UI, first runs only).
// `alone`: what this machine's game does at `frame` is its player's alone (Rollback::OnlineMatch
// AloneAt: it plays solo with nothing of drop-in under way or waiting to be taken, no friend
// plugged in or on the way, not joining or catching up, no keyframe wanted or being made, and no
// keyframe stored for a friend who may still arrive and replay from it; in harness runs, every
// first run). Always false on re-runs. It differs between machines: what a reader prints may depend
// on it (the app's "orca menu" lines), never what is written to memory.
using FrameCallback =
    std::function<void(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                       const std::vector<PortInfo>& ports, bool alone)>;
void SetFrameCallback(FrameCallback callback);

// This machine's game stopped following on from the frames its readers saw: it went solo after a
// session (whose first runs saw guessed inputs), or it loaded a friend's keyframe. A reader that
// tracks the game across frame boundaries (UX/OnlineMenu.h) takes the next boundary as its new
// start and announces nothing about what changed meanwhile. CPU thread; Resyncs() any thread.
void NoteResync();
u64 Resyncs();

// While the game waits on a friend (a stall), every few milliseconds on the CPU thread: how long so
// far. The overlay redraws the last frame from here (nothing is presented during a stall).
using StallCallback = std::function<void(int stalled_ms)>;
void SetStallCallback(StallCallback callback);

// Every frame boundary, solo or online, on the CPU thread: whether the frame that just ended was
// shown, a first run rendered as usual (not a re-run, nor a joiner's catch-up, which run unseen).
// The overlay's frame meter times these with the host's clock. Nothing here may touch emulation.
using BoundaryCallback = std::function<void(bool shown)>;
void SetBoundaryCallback(BoundaryCallback callback);

// The in-game UX's version of whatever it writes to emulated memory: part of the lobby
// compatibility key. Set before the game boots.
void SetUXCompatVersion(int version);
int UXCompatVersion();

// The session's side (Rollback / Online): publish the latest stats, and fire the plug-in callback.
void PublishLinkStats(const LinkStats& stats);
void NotifyPlugIn(int frame, const std::vector<PortInfo>& ports);
void NotifyFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                 const std::vector<PortInfo>& ports, bool alone);
void NotifyStall(int stalled_ms);
void NotifyBoundary(bool shown);
}  // namespace Orca::Events
