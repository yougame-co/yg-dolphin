// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Overlay.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <utility>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/HookableEvent.h"
#include "Core/Core.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Present.h"

namespace Orca::UX
{
namespace
{
// How far back the stall count decides the signal.
constexpr double STALL_WINDOW_MS = 3000;

double NowMs()
{
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Fields the session adds in its next push; until then the stall text stays off.
template <typename T>
int StalledMs(const T& stats)
{
  if constexpr (requires { stats.stalled_ms; })
    return stats.stalled_ms;
  else
    return -1;
}
template <typename T>
int WaitingSeat(const T& stats)
{
  if constexpr (requires { stats.waiting_seat; })
    return stats.waiting_seat;
  else
    return -1;
}

std::string NameOf(const Events::PortInfo& p)
{
  return p.name.empty() ? fmt::format("Player {}", p.port + 1) : p.name;
}

// The frame meter's colour for a readout: its frames shown in a second, and whether one of them was
// a hitch (past two frame periods) or late (past 20 ms).
Signal LevelOf(int fps, bool hitch, bool late)
{
  if (fps < OverlayModel::PERF_BAD_FPS || hitch)
    return Signal::Bad;
  if (fps < OverlayModel::PERF_OK_FPS || late)
    return Signal::Ok;
  return Signal::Good;
}

Signal Worse(Signal a, Signal b)
{
  return static_cast<int>(a) >= static_cast<int>(b) ? a : b;
}
}  // namespace

LinkView ViewOf(const Events::LinkStats& stats)
{
  LinkView v;
  v.online = stats.online;
  v.delay = stats.delay;
  v.rollbacks_per_second = stats.rollbacks_per_second;
  v.deepest_rollback = stats.deepest_rollback;
  v.stalls = stats.stalls;
  v.round_trip_ms = stats.round_trip_ms;
  v.stalled_ms = StalledMs(stats);
  v.waiting_seat = WaitingSeat(stats);
  return v;
}

void OverlayModel::OnPorts(int frame, const std::vector<Events::PortInfo>& ports, double now_ms)
{
  std::lock_guard lk(m_lock);
  // The session calls this on first runs only, when the list changes; a drop-in join can take the
  // frame number backwards (the host's frame), so frames don't order the lists.
  (void)frame;
  const auto find = [](const std::vector<Events::PortInfo>& list, int port) {
    return std::find_if(list.begin(), list.end(),
                        [port](const Events::PortInfo& p) { return p.port == port; });
  };
  const bool first = !m_seen_ports;
  m_seen_ports = true;
  for (const Events::PortInfo& p : ports)
  {
    if (!p.remote)
      continue;
    const auto was = find(m_ports, p.port);
    if (was != m_ports.end() && was->name == p.name && was->remote)
      continue;
    // A joiner's first list already has the host: they are on a port, not joining.
    m_toasts.emplace_back(now_ms, first ? fmt::format("{} is on port {}", NameOf(p), p.port + 1) :
                                          fmt::format("{} joined on port {}", NameOf(p), p.port + 1));
  }
  for (const Events::PortInfo& p : m_ports)
  {
    if (!p.remote)
      continue;
    const auto now = find(ports, p.port);
    if (now == ports.end() || now->name != p.name)
      m_toasts.emplace_back(now_ms, fmt::format("{} left", NameOf(p)));
  }
  m_ports = ports;
  while (m_toasts.size() > 3)
    m_toasts.pop_front();
}

void OverlayModel::Update(const LinkView& link, double now_ms)
{
  std::lock_guard lk(m_lock);
  m_link = link;
  m_stall_history.emplace_back(now_ms, link.stalls);
  while (m_stall_history.size() > 1 && m_stall_history.front().first < now_ms - STALL_WINDOW_MS)
    m_stall_history.pop_front();
  while (!m_toasts.empty() && m_toasts.front().first < now_ms - TOAST_MS)
    m_toasts.pop_front();
}

Signal OverlayModel::Quality(double) const
{
  const int recent_stalls = m_stall_history.empty() ?
                                0 :
                                m_stall_history.back().second - m_stall_history.front().second;
  const int rtt = m_link.round_trip_ms;
  if (m_link.stalled_ms > WAITING_AFTER_MS || rtt >= 150 || recent_stalls >= 10)
    return Signal::Bad;
  if (rtt >= 80 || recent_stalls > 0)
    return Signal::Ok;
  return Signal::Good;
}

double OverlayModel::RunningMs(double now_ms) const
{
  return (m_paused ? m_paused_at : now_ms) - m_paused_ms;
}

void OverlayModel::OnFrame(double now_ms, bool shown)
{
  std::lock_guard lk(m_lock);
  if (!shown)
  {
    ++m_unshown;
    return;
  }
  const double now = RunningMs(now_ms);
  // A long run of unseen frames is a joiner's catch-up, a load rather than the game's pace: the
  // meter starts over. A rollback's few re-runs only lengthen the next shown frame.
  if (std::exchange(m_unshown, 0) > PERF_MAX_UNSHOWN)
  {
    m_frames.clear();
    m_first_frame = -1;
    m_last_frame = -1;
  }
  if (m_first_frame < 0)
  {
    // The first shown boundary arms the meter, the second starts it.
    if (m_last_frame >= 0)
      m_first_frame = now;
  }
  else
  {
    m_frames.emplace_back(now, now - m_last_frame);
  }
  m_last_frame = now;
  while (!m_frames.empty() && m_frames.front().first <= now - PERF_WINDOW_MS - PERF_HOLD_MS)
    m_frames.pop_front();
}

void OverlayModel::Pause(double now_ms)
{
  std::lock_guard lk(m_lock);
  if (m_paused)
    return;
  m_paused = true;
  m_paused_at = now_ms;
}

void OverlayModel::Resume(double now_ms)
{
  std::lock_guard lk(m_lock);
  if (!m_paused)
    return;
  m_paused = false;
  m_paused_ms += std::max(0.0, now_ms - m_paused_at);
}

void OverlayModel::Meter(double now_ms, Stats stats, Perf perf, Frame* f) const
{
  // Paused, the readout stands still at the moment of the pause.
  const double now = RunningMs(now_ms);
  if (perf == Perf::Off || m_first_frame < 0 || now < m_first_frame + PERF_WINDOW_MS)
    return;
  // One pass, oldest first: the readout at each shown boundary of the last PERF_HOLD_MS gives the
  // colour its hold. `first` is the oldest frame of the second being read; `hitch` and `late` are
  // the newest frames past the thresholds (-1: none).
  std::size_t first = 0;
  std::ptrdiff_t hitch = -1, late = -1;
  const auto level_at = [&](std::size_t end, double t) {
    while (first < end && m_frames[first].first <= t - PERF_WINDOW_MS)
      ++first;
    const auto oldest = static_cast<std::ptrdiff_t>(first);
    return LevelOf(static_cast<int>(end - first), hitch >= oldest, late >= oldest);
  };
  Signal level = Signal::Good;
  std::size_t end = 0;
  for (; end < m_frames.size() && m_frames[end].first <= now; ++end)
  {
    const auto [at, ms] = m_frames[end];
    if (ms > PERF_BAD_FRAME_MS)
      hitch = static_cast<std::ptrdiff_t>(end);
    if (ms > PERF_OK_FRAME_MS)
      late = static_cast<std::ptrdiff_t>(end);
    if (at > now - PERF_HOLD_MS && at >= m_first_frame + PERF_WINDOW_MS)
      level = Worse(level, level_at(end + 1, at));
  }
  // Now: the last second's frames, and the frame still running (the game waiting for a friend)
  // is at least this long already.
  level_at(end, now);
  const int fps = static_cast<int>(end - first);
  double longest_ms = std::max(0.0, now - m_last_frame);
  for (std::size_t i = first; i < end; ++i)
    longest_ms = std::max(longest_ms, m_frames[i].second);
  f->perf_level = Worse(level, LevelOf(fps, longest_ms > PERF_BAD_FRAME_MS,
                                       longest_ms > PERF_OK_FRAME_MS));
  f->perf = fmt::format("{} fps · {:.0f} ms", fps, longest_ms);
  if (perf != Perf::Detailed)
    return;
  // The ping line says these already with `ping full`.
  if (m_link.online && stats != Stats::Full)
    f->perf += fmt::format(" · delay {} · {:.1f} rb/s", m_link.delay, m_link.rollbacks_per_second);
  for (std::size_t i = 0; i < end; ++i)
  {
    if (m_frames[i].first > now - PERF_GRAPH_MS)
      f->graph.push_back(static_cast<float>(m_frames[i].second));
  }
}

OverlayModel::Frame OverlayModel::View(double now_ms, Stats stats, Perf perf) const
{
  std::lock_guard lk(m_lock);
  Frame f;
  f.show_signal = m_link.online;
  f.signal = Quality(now_ms);
  if (m_link.online && m_link.stalled_ms > WAITING_AFTER_MS)
  {
    std::string who = "your friend";
    for (const Events::PortInfo& p : m_ports)
    {
      if (p.port == m_link.waiting_seat && p.remote)
        who = NameOf(p);
    }
    f.waiting = fmt::format("Waiting for {}…", who);
  }
  for (const auto& [at, text] : m_toasts)
  {
    const double left = at + TOAST_MS - now_ms;
    if (left <= 0)
      continue;
    f.toasts.push_back(
        {text, static_cast<float>(std::clamp(left / TOAST_FADE_MS, 0.0, 1.0))});
  }
  if (stats != Stats::Off && m_link.online)
  {
    const std::string ping = m_link.round_trip_ms >= 0 ?
                                 fmt::format("{} ms", m_link.round_trip_ms) :
                                 std::string("– ms");
    f.stats = stats == Stats::Full ? fmt::format("{} · delay {} · {:.1f} rb/s", ping,
                                                 m_link.delay, m_link.rollbacks_per_second) :
                                     ping;
  }
  Meter(now_ms, stats, perf, &f);
  return f;
}

namespace
{
OverlayModel s_overlay;
std::atomic<Stats> s_stats{Stats::Off};
// On until the app says otherwise (Luis wants it while testing).
std::atomic<Perf> s_perf{Perf::Fps};
Common::EventHook s_state_hook;  // the core's pause and resume, for the frame meter
std::atomic<bool> s_visible{true};  // the app shows the game (embed hide/show)
// ORCA_UX_OVERLAY_DEMO=1 (design checks only): a made-up friend whose link cycles good, ok, bad
// with a stall, every 12 s, in place of the session's stats.
bool s_demo = false;
double s_demo_start = -1;

LinkView DemoLink(double now)
{
  if (s_demo_start < 0)
  {
    s_demo_start = now;
    s_overlay.OnPorts(0, {{0, "luis", false}}, now);
    s_overlay.OnPorts(1, {{0, "luis", false}, {1, "ada", true}}, now);
  }
  const double t = std::fmod(now - s_demo_start, 12000);
  LinkView v;
  v.online = true;
  v.delay = 2;
  v.rollbacks_per_second = t < 4000 ? 0.8 : 6.5;
  v.round_trip_ms = t < 4000 ? 35 : t < 8000 ? 95 : 180;
  v.stalled_ms = t >= 8000 ? static_cast<int>(t - 8000) : 0;
  v.waiting_seat = 1;
  return v;
}

ImU32 Rgba(int r, int g, int b, float a)
{
  return IM_COL32(r, g, b, static_cast<int>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255)));
}

// The frame meter's colours: white while the game keeps its pace, then the circle's yellow and red.
ImU32 LevelColour(Signal level, float alpha)
{
  return level == Signal::Bad ? Rgba(255, 72, 64, alpha) :
         level == Signal::Ok  ? Rgba(255, 208, 64, alpha) :
                                Rgba(255, 255, 255, alpha);
}

// Text the way Brawl's HUD sets it: white, with a dark outline so it reads on any stage.
void OutlinedText(ImDrawList* dl, float size, ImVec2 pos, float alpha, const std::string& text,
                  Signal level = Signal::Good)
{
  ImFont* font = ImGui::GetFont();
  const float o = std::max(1.0f, size / 14);
  for (const ImVec2 d : {ImVec2(-o, 0), ImVec2(o, 0), ImVec2(0, -o), ImVec2(0, o), ImVec2(o, o)})
    dl->AddText(font, size, ImVec2(pos.x + d.x, pos.y + d.y), Rgba(16, 16, 24, alpha * 0.85f),
                text.c_str());
  dl->AddText(font, size, pos, LevelColour(level, alpha), text.c_str());
}

ImVec2 TextSize(float size, const std::string& text)
{
  return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
}

// Brawl's Wi-Fi connection circle: a dark disc with a dot and three arcs, lit by quality.
void SignalCircle(ImDrawList* dl, ImVec2 c, float r, Signal signal)
{
  const int lit = signal == Signal::Good ? 3 : signal == Signal::Ok ? 2 : 1;
  const ImU32 on = signal == Signal::Good ? Rgba(80, 200, 255, 1) :
                   signal == Signal::Ok   ? Rgba(255, 208, 64, 1) :
                                            Rgba(255, 72, 64, 1);
  const ImU32 off = Rgba(255, 255, 255, 0.22f);
  dl->AddCircleFilled(c, r, Rgba(10, 14, 28, 0.62f), 40);
  dl->AddCircle(c, r, Rgba(255, 255, 255, 0.55f), 40, std::max(1.0f, r * 0.07f));
  const ImVec2 base(c.x, c.y + r * 0.45f);
  dl->AddCircleFilled(base, r * 0.12f, on, 16);
  const float pi = std::numbers::pi_v<float>;
  for (int i = 0; i < 3; ++i)
  {
    const float radius = r * (0.32f + 0.22f * static_cast<float>(i));
    dl->PathArcTo(base, radius, pi * 1.25f, pi * 1.75f, 16);
    dl->PathStroke(i < lit ? on : off, ImDrawFlags_None, r * 0.12f);
  }
}

// The frame meter's graph: a bar per shown frame of the last two seconds, newest on the right, up
// to 50 ms tall, with a line at one frame period.
void FrameGraph(ImDrawList* dl, ImVec2 pos, ImVec2 size, const std::vector<float>& frames)
{
  constexpr float TOP_MS = 50;
  constexpr float SLOTS = OverlayModel::PERF_GRAPH_MS * 60 / 1000;
  dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), Rgba(10, 14, 28, 0.62f),
                    size.y * 0.12f);
  const float slot = size.x / SLOTS;
  // Bars a few pixels wide get a gap; thinner ones touch (a gap would alias them away).
  const float gap = slot >= 3 ? slot * 0.2f : 0;
  const float bottom = pos.y + size.y;
  float x = pos.x + size.x;
  for (auto it = frames.rbegin(); it != frames.rend() && x - slot >= pos.x - 0.5f; ++it)
  {
    const float ms = *it;
    const Signal level = ms > OverlayModel::PERF_BAD_FRAME_MS ? Signal::Bad :
                         ms > OverlayModel::PERF_OK_FRAME_MS  ? Signal::Ok :
                                                                Signal::Good;
    const float h = std::max(1.0f, std::min(ms, TOP_MS) / TOP_MS * size.y);
    dl->AddRectFilled(ImVec2(x - slot, bottom - h), ImVec2(x - gap, bottom),
                      level == Signal::Good ? Rgba(170, 200, 255, 0.7f) : LevelColour(level, 1));
    x -= slot;
  }
  const float period_y = bottom - 1000.0f / 60 / TOP_MS * size.y;
  dl->AddLine(ImVec2(pos.x, period_y), ImVec2(pos.x + size.x, period_y),
              Rgba(255, 255, 255, 0.35f), std::max(1.0f, size.y / 40));
}

void Draw()
{
  const double now = NowMs();
  s_overlay.Update(s_demo ? DemoLink(now) : ViewOf(Events::GetLinkStats()), now);
  const OverlayModel::Frame f = s_overlay.View(now, s_stats.load(std::memory_order_relaxed),
                                               s_perf.load(std::memory_order_relaxed));
  if (!f.show_signal && f.waiting.empty() && f.toasts.empty() && f.stats.empty() &&
      f.perf.empty())
  {
    return;
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  const float unit = screen.y / 100;  // 1% of the height: the overlay scales with the window
  const float margin = 3 * unit;
  const float text_size = 3.0f * unit;
  // Top left, where Brawl's matches and menus leave the screen empty: the circle, the ping beside
  // it on the circle's centre line (the frame meter under it), the meter alone when solo, its
  // graph, then the toasts under them.
  float y = margin;
  if (f.show_signal)
  {
    const float r = 3.0f * unit;
    const ImVec2 c(margin + r, margin + r);
    SignalCircle(dl, c, r, f.signal);
    // The first line on the circle's centre line, as the ping alone always sat; the meter under it.
    const float x = c.x + r + unit;
    const float line = TextSize(text_size, "0").y;
    float bottom = c.y + r;
    float text_y = c.y - line / 2;
    if (!f.stats.empty())
    {
      OutlinedText(dl, text_size, ImVec2(x, text_y), 1, f.stats);
      text_y += line;
    }
    if (!f.perf.empty())
    {
      OutlinedText(dl, text_size, ImVec2(x, text_y), 1, f.perf, f.perf_level);
      bottom = std::max(bottom, text_y + line);
    }
    y = bottom + 1.5f * unit;
  }
  else if (!f.perf.empty())
  {
    OutlinedText(dl, text_size, ImVec2(margin, y), 1, f.perf, f.perf_level);
    y += TextSize(text_size, f.perf).y + unit;
  }
  if (!f.graph.empty())
  {
    const ImVec2 size(40 * unit, 8 * unit);
    FrameGraph(dl, ImVec2(margin, y), size, f.graph);
    y += size.y + 1.5f * unit;
  }
  if (!f.waiting.empty())
  {
    const float size = 4.4f * unit;
    const ImVec2 s = TextSize(size, f.waiting);
    OutlinedText(dl, size, ImVec2((screen.x - s.x) / 2, screen.y * 0.18f), 1, f.waiting);
  }
  for (const OverlayModel::Toast& t : f.toasts)
  {
    OutlinedText(dl, text_size, ImVec2(margin, y), t.alpha, t.text);
    y += TextSize(text_size, t.text).y + unit;
  }
}
}  // namespace

OverlayModel& Overlay()
{
  return s_overlay;
}

void SetStats(Stats stats)
{
  s_stats = stats;
}

void SetPerf(Perf perf)
{
  s_perf = perf;
}

void FrameBoundary(bool shown)
{
  s_overlay.OnFrame(NowMs(), shown);
}

void ShutdownOverlay()
{
  OSD::SetHostOverlay({});
  s_state_hook.reset();
}

void SetHostVisible(bool visible)
{
  s_visible = visible;
}

void RepresentDuringStall(int stalled_ms)
{
  // Sessions run single core, so this presents on the CPU thread inside the stall wait, and while
  // it presents the wait hears nothing (the friend's input, a stop). So: nothing for a short stall
  // (the overlay has nothing to say before 250 ms), nothing while the app hides the game or its
  // window is out of sight, ~15 times a second, 4 when presenting is slow, and not again in this
  // stall once one present took over 50 ms (vsync can hold a Metal drawable for a while).
  static double s_last = 0;
  static double s_every = 66;
  static int s_stall_ms = 0;
  static bool s_off_this_stall = false;
  if (stalled_ms < s_stall_ms)
    s_off_this_stall = false;  // a new stall
  s_stall_ms = stalled_ms;
  if (s_off_this_stall || !s_visible.load(std::memory_order_relaxed) ||
      !VideoCommon::Presenter::IsSurfaceVisible() ||
      stalled_ms < OverlayModel::WAITING_AFTER_MS - 50)
  {
    return;
  }
  const double now = NowMs();
  if (now - s_last < s_every)
    return;
  AsyncRequests::GetInstance()->PushEvent([] {
    if (g_presenter)
      g_presenter->RepresentLast();
  });
  const double took = NowMs() - now;
  s_every = took > 12 ? 250 : 66;
  if (took > 50)
    s_off_this_stall = true;
  s_last = NowMs();
}

void InitOverlay()
{
  if (const char* v = std::getenv("ORCA_UX_STATS"); v && (v[0] == '1' || v[0] == '2'))
    s_stats = v[0] == '2' ? Stats::Full : Stats::Ping;
  if (const char* v = std::getenv("ORCA_UX_OVERLAY_DEMO"); v && v[0] == '1')
    s_demo = true;
  // Paused, the frame meter stands still (a pause is no slow frame), on whichever thread pauses.
  s_state_hook = Core::AddOnStateChangedCallback([](Core::State state) {
    if (state == Core::State::Paused)
      s_overlay.Pause(NowMs());
    else if (state == Core::State::Running)
      s_overlay.Resume(NowMs());
  });
  OSD::SetHostOverlay(&Draw);
}
}  // namespace Orca::UX
