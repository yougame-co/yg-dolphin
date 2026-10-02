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

#include <fmt/format.h>
#include <imgui.h>

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
  if (m_seen_ports && frame <= m_last_frame)
    return;
  m_last_frame = frame;
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

OverlayModel::Frame OverlayModel::View(double now_ms, bool show_stats) const
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
  if (show_stats && m_link.online)
  {
    f.stats = fmt::format("{} · delay {} · {:.1f} rb/s",
                          m_link.round_trip_ms >= 0 ? fmt::format("{} ms", m_link.round_trip_ms) :
                                                      std::string("– ms"),
                          m_link.delay, m_link.rollbacks_per_second);
  }
  return f;
}

namespace
{
OverlayModel s_overlay;
std::atomic<bool> s_show_stats{false};
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

// Text the way Brawl's HUD sets it: white, with a dark outline so it reads on any stage.
void OutlinedText(ImDrawList* dl, float size, ImVec2 pos, float alpha, const std::string& text)
{
  ImFont* font = ImGui::GetFont();
  const float o = std::max(1.0f, size / 14);
  for (const ImVec2 d : {ImVec2(-o, 0), ImVec2(o, 0), ImVec2(0, -o), ImVec2(0, o), ImVec2(o, o)})
    dl->AddText(font, size, ImVec2(pos.x + d.x, pos.y + d.y), Rgba(16, 16, 24, alpha * 0.85f),
                text.c_str());
  dl->AddText(font, size, pos, Rgba(255, 255, 255, alpha), text.c_str());
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

void Draw()
{
  const double now = NowMs();
  s_overlay.Update(s_demo ? DemoLink(now) : ViewOf(Events::GetLinkStats()), now);
  const OverlayModel::Frame f = s_overlay.View(now, s_show_stats.load(std::memory_order_relaxed));
  if (!f.show_signal && f.waiting.empty() && f.toasts.empty() && f.stats.empty())
    return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  const float unit = screen.y / 100;  // 1% of the height: the overlay scales with the window
  const float margin = 3 * unit;
  float right_y = margin;
  if (f.show_signal)
  {
    const float r = 3.0f * unit;
    SignalCircle(dl, ImVec2(screen.x - margin - r, margin + r), r, f.signal);
    right_y += 2 * r + unit;
  }
  if (!f.stats.empty())
  {
    const float size = 2.4f * unit;
    const ImVec2 s = TextSize(size, f.stats);
    OutlinedText(dl, size, ImVec2(screen.x - margin - s.x, right_y), 0.9f, f.stats);
  }
  if (!f.waiting.empty())
  {
    const float size = 4.4f * unit;
    const ImVec2 s = TextSize(size, f.waiting);
    OutlinedText(dl, size, ImVec2((screen.x - s.x) / 2, screen.y * 0.18f), 1, f.waiting);
  }
  float y = margin;
  for (const OverlayModel::Toast& t : f.toasts)
  {
    const float size = 3.0f * unit;
    OutlinedText(dl, size, ImVec2(margin, y), t.alpha, t.text);
    y += TextSize(size, t.text).y + unit;
  }
}
}  // namespace

OverlayModel& Overlay()
{
  return s_overlay;
}

void SetStatsVisible(bool visible)
{
  s_show_stats = visible;
}

void ShutdownOverlay()
{
  OSD::SetHostOverlay({});
}

void RepresentDuringStall()
{
  static double s_last = 0;
  const double now = NowMs();
  if (now - s_last < 33)
    return;
  s_last = now;
  AsyncRequests::GetInstance()->PushEvent([] {
    if (g_presenter)
      g_presenter->RepresentLast();
  });
}

void InitOverlay()
{
  if (const char* v = std::getenv("ORCA_UX_STATS"); v && v[0] == '1')
    s_show_stats = true;
  if (const char* v = std::getenv("ORCA_UX_OVERLAY_DEMO"); v && v[0] == '1')
    s_demo = true;
  OSD::SetHostOverlay(&Draw);
}
}  // namespace Orca::UX
