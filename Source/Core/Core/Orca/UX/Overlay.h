// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "Core/Orca/Session/Events.h"

// The minimal online overlay (ORCA_ONLINE_UX.md 1D), only what the game has nothing native for:
//   - a small connection circle in the top-left corner while a friend is plugged in, styled after
//     Brawl's own Wi-Fi connection circle: 3 arcs blue, 2 yellow, 1 red;
//   - "Waiting for <name>…" while the game has been waiting for a friend's input for 250 ms;
//   - short toasts, "<name> joined on port N" and "<name> left", under the circle;
//   - beside the circle, the ping: "32 ms" with embed `ping on` (the YouGame page's default) or
//     ORCA_UX_STATS=1; with `ping full` or ORCA_UX_STATS=2, also the delay and rollbacks/s;
//   - the frame meter, solo too: "60 fps · 17 ms", the frames shown in the last second and the
//     longest of them, coloured while the game misses its pace (embed `perf`, on until it says);
//   - the page's chat while Orca fills the screen (embed `chat`, Chat.h): "<name>: <text>" for 8 s,
//     wrapped to under half the width, the last three, under the toasts.
// It is drawn by the host's OSD layer over the presented image (ImGui), never into the emulated
// framebuffer, and reads no game state: nothing in it can change emulation or desync.
namespace Orca::UX
{
// What the readout beside the circle says.
enum class Stats
{
  Off,
  Ping,  // "32 ms"
  Full,  // "32 ms · delay 2 · 1.2 rb/s"
};

// What the frame meter shows (embed `perf off|fps|detailed`; `fps` until the app says).
enum class Perf
{
  Off,
  Fps,       // "60 fps · 17 ms"
  Detailed,  // also the last 2 s of frame times as a graph, and online the delay and rollbacks/s
};

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
  int round_trip_ms = -1;  // the input path's: the direct link's when one is up, else the relay's
  int transport = 0;       // 0 relay, 1 direct, 2 TURN (Events::LinkStats::transport)
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
  // The frame meter: fps and the longest frame over the last second, the graph over two. Coloured
  // (Ok) under 59 fps or past 20 ms a frame; Bad under 50 fps or past two frame periods (a hitch,
  // as OnlineMatch's HITCH_TIME counts them). The colour is the worst of the readouts at the
  // boundaries of the last two seconds, so a single hitch stays coloured that much longer.
  static constexpr double PERF_WINDOW_MS = 1000;
  static constexpr double PERF_GRAPH_MS = 2000;
  static constexpr double PERF_HOLD_MS = 2000;
  static constexpr int PERF_OK_FPS = 59;
  static constexpr double PERF_OK_FRAME_MS = 20;
  static constexpr int PERF_BAD_FPS = 50;
  static constexpr double PERF_BAD_FRAME_MS = 2000.0 / 60;
  // More unseen frames in a row than any rollback re-runs: a joiner's catch-up.
  static constexpr int PERF_MAX_UNSHOWN = 60;

  // A chat line (Chat.h) stays this long, fading like a toast, and the newest CHAT_KEEP show.
  static constexpr double CHAT_MS = 8000;
  static constexpr std::size_t CHAT_KEEP = 3;

  struct Toast
  {
    std::string text;
    float alpha;
  };
  struct ChatToast
  {
    std::string name;
    std::string text;
    float alpha;
  };
  struct Frame
  {
    bool show_signal = false;
    Signal signal = Signal::Good;
    std::string waiting;  // empty: not shown
    std::vector<Toast> toasts;
    std::vector<ChatToast> chats;  // oldest first
    std::string stats;  // empty: not shown
    std::string perf;   // the frame meter; empty: not shown
    Signal perf_level = Signal::Good;  // Good: plain text
    std::vector<float> graph;          // Detailed: the last 2 s of frame times (ms), oldest first
  };

  // The session's plug-in callback (any thread; first runs only, when the list changes). A list equal
  // to the last one shows nothing new.
  void OnPorts(int frame, const std::vector<Events::PortInfo>& ports, double now_ms);
  // A toast of its own (a matchmade game's result: "You won game 2 · 1–1"). Any thread.
  void AddToast(std::string text, double now_ms);
  // A line of the page's chat (embed `chat`, Chat.h), already decoded. Any thread.
  void AddChat(std::string name, std::string text, double now_ms);
  // The newest link stats (the drawing thread, each frame).
  void Update(const LinkView& link, double now_ms);
  // Every frame boundary (the CPU thread): `shown` says the frame that just ended ran rendered, a
  // first run (not a re-run, nor a joiner's catch-up). A frame's time is the host's wall time since
  // the last shown frame's boundary, so re-runs in between lengthen it but never count as frames.
  // The first shown boundary only arms the meter: the one before it can hold a wait (a session's
  // first boundary waits for the room's ticket).
  void OnFrame(double now_ms, bool shown);
  // The core paused / resumed (the host thread, around the CPU's own stop and start): the meter's
  // clock stands still meanwhile, so a boundary that lands between a resume and its callback, or
  // between a pause's callback and the CPU stopping, times only the running part of its frame.
  void Pause(double now_ms);
  void Resume(double now_ms);
  Frame View(double now_ms, Stats stats, Perf perf = Perf::Off) const;

private:
  Signal Quality(double now_ms) const;
  void Meter(double now_ms, Stats stats, Perf perf, Frame* f) const;
  // The meter's clock: host time less the time paused, standing still while paused.
  double RunningMs(double now_ms) const;

  mutable std::mutex m_lock;
  bool m_seen_ports = false;
  std::vector<Events::PortInfo> m_ports;
  std::deque<std::pair<double, std::string>> m_toasts;
  struct ChatEntry
  {
    double at;
    std::string name;
    std::string text;
  };
  std::deque<ChatEntry> m_chats;
  LinkView m_link;
  std::deque<std::pair<double, int>> m_stall_history;  // (time, total stalls)
  // The frame meter, on its own clock (RunningMs): shown frames (boundary, frame time ms) of the
  // last PERF_WINDOW_MS + PERF_HOLD_MS, enough to give the readout at every boundary the colour
  // holds for.
  std::deque<std::pair<double, double>> m_frames;
  double m_first_frame = -1;  // where the meter starts: no readout for its first second
  double m_last_frame = -1;   // the last shown boundary (-1: not armed)
  int m_unshown = 0;          // unseen frames since it
  bool m_paused = false;
  double m_paused_at = 0;     // host time
  double m_paused_ms = 0;     // host time paused, pauses that ended
};

// The process's overlay: Init() registers it with the OSD layer and the session.
void InitOverlay();
void ShutdownOverlay();
// While the game waits for a friend the CPU thread is blocked, so no frame is presented and the
// overlay could not say so: the session calls this from its stall wait (CPU thread), and once the
// stall nears 250 ms the last frame is shown again with the overlay on it, about 15 times a second.
// Host-only drawing: the emulated state is untouched.
void RepresentDuringStall(int stalled_ms);
// The app hid or showed the game (embed `hide` / `show`): no stall redraws while hidden.
void SetHostVisible(bool visible);
void SetStats(Stats stats);
void SetPerf(Perf perf);
// The game's frame boundary (Orca::Events' boundary callback), for the frame meter.
void FrameBoundary(bool shown);
// A short toast under the circle (OverlayModel::AddToast, now). Any thread.
void ShowToast(std::string text);
// A line of the page's chat (OverlayModel::AddChat, now). Never logged. Any thread.
void ShowChat(std::string name, std::string text);
// A ranked set's lines (BrawlStages.h), top centre: the set score, and under it whose turn it is
// on the stage select with its timer. Empty: not shown. Any thread (the frame hook, first runs).
void SetRulesLines(std::string line, std::string note);
// The character order's line (CharOrder.h), under the set's score on the character select ("Ada
// picks first · 0:42"); empty: none. Display only: computed from memory on first runs. Any thread.
void SetRuleLine(std::string text);
// The queue's character select (Queue.h), top centre in place of the search's lines: "Casual ·
// Searching for an opponent…", "ada locked in · Your pick: Start locks it in"; each port's label
// near its panel at the bottom ("ada · 1532"); and a LOCKED IN badge in the port's colour over the
// panel of each player who has locked in (`locked`). Empty: not shown. Display only: computed from
// memory on first runs. Any thread.
void SetQueueLines(std::string line, std::string note, std::array<std::string, 4> labels,
                   std::array<bool, 4> locked = {});
// The colour of port `port` (0-3), as the games draw their panels: red, blue, yellow, green.
struct PortColour
{
  int r, g, b;
};
PortColour PortColourOf(int port);
OverlayModel& Overlay();
// The overlay's unit, 1% of the picture's height (it scales with the window), or 0 below
// OVERLAY_MIN_HEIGHT pixels: then it draws nothing. ImGui rounds a text size to whole pixels and
// aborts on 0, which a window under ~17 px tall (the app's player box scrolled nearly out of view)
// used to reach, and nothing would read in a sliver that thin anyway.
constexpr float OVERLAY_MIN_HEIGHT = 100;
float OverlayUnit(float display_height);
}  // namespace Orca::UX
