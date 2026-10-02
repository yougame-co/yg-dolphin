// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "Core/Orca/Session/Events.h"

// The minimal online overlay (ORCA_ONLINE_UX.md 1D), only what the game has nothing native for:
//   - a small connection circle in the corner while a friend is plugged in, styled after Brawl's
//     own Wi-Fi connection circle: 3 arcs blue, 2 yellow, 1 red;
//   - "Waiting for <name>…" while the game has been waiting for a friend's input for 250 ms;
//   - short toasts, "<name> joined on port N" and "<name> left";
//   - optionally (embed `ping on`, or ORCA_UX_STATS=1) a one-line ping/delay readout.
// It is drawn by the host's OSD layer over the presented image (ImGui), never into the emulated
// framebuffer, and reads no game state: nothing in it can change emulation or desync.
namespace Orca::UX
{
enum class Signal
{
  Good,
  Ok,
  Bad,
};

// The link as the overlay needs it (from Events::LinkStats).
struct LinkView
{
  bool online = false;
  int delay = 0;
  double rollbacks_per_second = 0;
  int deepest_rollback = 0;
  int stalls = 0;  // frames waited, in total
  int round_trip_ms = -1;
  int stalled_ms = 0;     // the current stall (0: none); -1: the session doesn't say
  int waiting_seat = -1;  // whose input it waits for (seat s plays on GameCube port s)
};
LinkView ViewOf(const Events::LinkStats& stats);

// What one frame of the overlay shows. Pure state machine, so the tests drive it with a clock.
class OverlayModel
{
public:
  static constexpr double TOAST_MS = 3000;
  static constexpr double TOAST_FADE_MS = 500;
  static constexpr int WAITING_AFTER_MS = 250;

  struct Toast
  {
    std::string text;
    float alpha;
  };
  struct Frame
  {
    bool show_signal = false;
    Signal signal = Signal::Good;
    std::string waiting;  // empty: not shown
    std::vector<Toast> toasts;
    std::string stats;  // empty: not shown
  };

  // The session's plug-in callback (any thread). A re-run of a plug-in frame repeats its list: a
  // list from a frame at or before the last one handled shows nothing.
  void OnPorts(int frame, const std::vector<Events::PortInfo>& ports, double now_ms);
  // The newest link stats (the drawing thread, each frame).
  void Update(const LinkView& link, double now_ms);
  Frame View(double now_ms, bool show_stats) const;

private:
  Signal Quality(double now_ms) const;

  mutable std::mutex m_lock;
  bool m_seen_ports = false;
  int m_last_frame = -1;
  std::vector<Events::PortInfo> m_ports;
  std::deque<std::pair<double, std::string>> m_toasts;
  LinkView m_link;
  std::deque<std::pair<double, int>> m_stall_history;  // (time, total stalls)
};

// The process's overlay: Init() registers it with the OSD layer and the session.
void InitOverlay();
void ShutdownOverlay();
// While the game waits for a friend the CPU thread is blocked, so no frame is presented and the
// overlay could not say so: the session calls this from its stall wait (CPU thread), and once the
// stall nears 250 ms the last frame is shown again with the overlay on it, about 15 times a second.
// Host-only drawing: the emulated state is untouched.
void RepresentDuringStall(int stalled_ms);
void SetStatsVisible(bool visible);
OverlayModel& Overlay();
}  // namespace Orca::UX
