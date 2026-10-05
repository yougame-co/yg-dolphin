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
#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/OnlineMenu.h"
#include "Core/Orca/UX/RankedPPlus.h"
#include "Core/Orca/UX/RankedSet.h"
#include "Core/Orca/UX/SetEnd.h"
#include "Core/Orca/UX/StageCursors.h"
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
  // Over a direct link (or TURN) the inputs travel that link, so its round trip is the ping that
  // matters (as Slippi shows); the relay's stays the ping until the link is up.
  v.transport = stats.link_rtt_ms >= 0 ? stats.transport : 0;
  v.round_trip_ms = v.transport > 0 ? stats.link_rtt_ms : stats.round_trip_ms;
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

void OverlayModel::AddToast(std::string text, double now_ms)
{
  std::lock_guard lk(m_lock);
  m_toasts.emplace_back(now_ms, std::move(text));
  while (m_toasts.size() > 3)
    m_toasts.pop_front();
}

void OverlayModel::AddChat(std::string name, std::string text, double now_ms)
{
  std::lock_guard lk(m_lock);
  m_chats.push_back({now_ms, std::move(name), std::move(text)});
  while (m_chats.size() > CHAT_KEEP)
    m_chats.pop_front();
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
  while (!m_chats.empty() && m_chats.front().at < now_ms - CHAT_MS)
    m_chats.pop_front();
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
  for (const ChatEntry& c : m_chats)
  {
    const double left = c.at + CHAT_MS - now_ms;
    if (left <= 0)
      continue;
    f.chats.push_back(
        {c.name, c.text, static_cast<float>(std::clamp(left / TOAST_FADE_MS, 0.0, 1.0))});
  }
  if (stats != Stats::Off && m_link.online)
  {
    const std::string ping = m_link.round_trip_ms >= 0 ?
                                 fmt::format("{} ms", m_link.round_trip_ms) :
                                 std::string("– ms");
    const char* path = m_link.transport == 1 ? " direct" : m_link.transport == 2 ? " TURN" : "";
    f.stats = stats == Stats::Full ? fmt::format("{}{} · delay {} · {:.1f} rb/s", ping, path,
                                                 m_link.delay, m_link.rollbacks_per_second) :
                                     ping;
  }
  Meter(now_ms, stats, perf, &f);
  return f;
}

namespace
{
std::mutex s_rules_lock;
std::string s_rules_line;
std::string s_rules_note;
OverlayModel s_overlay;
std::atomic<Stats> s_stats{Stats::Off};
// On until the app says otherwise (Luis wants it while testing).
std::atomic<Perf> s_perf{Perf::Fps};
Common::EventHook s_state_hook;  // the core's pause and resume, for the frame meter
std::atomic<bool> s_visible{true};  // the app shows the game (embed hide/show)
std::mutex s_rule_line_lock;
std::string s_rule_line;  // SetRuleLine's
std::mutex s_queue_lock;
std::string s_queue_line, s_queue_note;  // SetQueueLines'
std::array<std::string, 4> s_queue_labels;
std::array<bool, 4> s_queue_locked{};
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

// The size every text below hands ImGui: at least 1 px. ImGui rounds a size to whole pixels and
// asserts on 0 (ImFontAtlasBakedGetOrAdd: font_size > 0.0f), which Orca 0.3.13 trapped on in a
// window under ~17 px tall; !(>=) also turns a NaN away. Draw already returns in a picture that
// thin (OverlayUnit): this keeps any size it computes from reaching ImGui under a pixel.
float TextPx(float size)
{
  return size >= 1.0f ? size : 1.0f;
}

// Text the way Brawl's HUD sets it: white, with a dark outline so it reads on any stage.
void OutlinedText(ImDrawList* dl, float size, ImVec2 pos, float alpha, const std::string& text,
                  Signal level = Signal::Good)
{
  ImFont* font = ImGui::GetFont();
  size = TextPx(size);
  const float o = std::max(1.0f, size / 14);
  for (const ImVec2 d : {ImVec2(-o, 0), ImVec2(o, 0), ImVec2(0, -o), ImVec2(0, o), ImVec2(o, o)})
    dl->AddText(font, size, ImVec2(pos.x + d.x, pos.y + d.y), Rgba(16, 16, 24, alpha * 0.85f),
                text.c_str());
  dl->AddText(font, size, pos, LevelColour(level, alpha), text.c_str());
}

ImVec2 TextSize(float size, const std::string& text)
{
  return ImGui::GetFont()->CalcTextSizeA(TextPx(size), FLT_MAX, 0, text.c_str());
}

// A chat line, "<name>: <text>" wrapped at `wrap` pixels, outlined like the rest, with the name in
// the circle's blue (drawn over the white in the same place: a prefix wraps as it does in the whole
// line). Returns its height.
float ChatLine(ImDrawList* dl, float size, ImVec2 pos, float wrap, const OverlayModel::ChatToast& c)
{
  const std::string prefix = c.name + ":";
  const std::string line = prefix + " " + c.text;
  ImFont* font = ImGui::GetFont();
  size = TextPx(size);
  const float o = std::max(1.0f, size / 14);
  for (const ImVec2 d : {ImVec2(-o, 0), ImVec2(o, 0), ImVec2(0, -o), ImVec2(0, o), ImVec2(o, o)})
  {
    dl->AddText(font, size, ImVec2(pos.x + d.x, pos.y + d.y), Rgba(16, 16, 24, c.alpha * 0.85f),
                line.c_str(), nullptr, wrap);
  }
  dl->AddText(font, size, pos, Rgba(255, 255, 255, c.alpha), line.c_str(), nullptr, wrap);
  dl->AddText(font, size, pos, Rgba(80, 200, 255, c.alpha), prefix.c_str(), nullptr, wrap);
  return font->CalcTextSizeA(size, FLT_MAX, wrap, line.c_str()).y;
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

// The centre of port `port`'s panel on the character select, as a fraction of the game's picture's
// width (both games, widescreen: ports 1 and 2 at 0.236 and 0.413 of a 1280x720 picture, 0.177
// apart).
float PanelCentre(int port)
{
  return 0.236f + 0.177f * static_cast<float>(port);
}

// Where the LOCKED IN badge sits, as a fraction of the picture's height: across the panel's
// picture, under the name label.
constexpr float LOCKED_BADGE_Y = 0.71f;

// A player's LOCKED IN badge, centred on `centre`: a pill in the port's colour with a white rim and
// white bold-looking capitals, so it reads over any character's portrait.
void LockedBadge(ImDrawList* dl, float unit, ImVec2 centre, PortColour colour)
{
  static const std::string text = "LOCKED IN";
  const float size = TextPx(4.6f * unit);
  const ImVec2 s = TextSize(size, text);
  const float pad_x = 1.8f * unit, pad_y = 0.8f * unit;
  const ImVec2 min(centre.x - s.x / 2 - pad_x, centre.y - s.y / 2 - pad_y);
  const ImVec2 max(centre.x + s.x / 2 + pad_x, centre.y + s.y / 2 + pad_y);
  const float round = (max.y - min.y) / 2;
  dl->AddRectFilled(ImVec2(min.x + 0.4f * unit, min.y + 0.5f * unit),
                    ImVec2(max.x + 0.4f * unit, max.y + 0.5f * unit), Rgba(0, 0, 0, 0.45f), round);
  dl->AddRectFilled(min, max, Rgba(colour.r, colour.g, colour.b, 0.95f), round);
  dl->AddRect(min, max, Rgba(255, 255, 255, 0.95f), round, ImDrawFlags_None,
              std::max(1.0f, 0.45f * unit));
  // A heavier face from the same font: the text drawn twice, a hair apart.
  const ImVec2 at(centre.x - s.x / 2, centre.y - s.y / 2);
  ImFont* font = ImGui::GetFont();
  const float o = std::max(1.0f, size / 14);
  for (const ImVec2 d : {ImVec2(-o, 0), ImVec2(o, 0), ImVec2(0, -o), ImVec2(0, o), ImVec2(o, o)})
    dl->AddText(font, size, ImVec2(at.x + d.x, at.y + d.y), Rgba(16, 16, 24, 0.7f), text.c_str());
  dl->AddText(font, size, at, Rgba(255, 255, 255, 1), text.c_str());
  dl->AddText(font, size, ImVec2(at.x + std::max(1.0f, size / 24), at.y), Rgba(255, 255, 255, 1),
              text.c_str());
}

// A port's colour (PortColourOf: the games' red, blue, yellow, green) at `alpha`.
ImU32 PortRgba(int port, float alpha)
{
  const PortColour c = PortColourOf(port);
  return Rgba(c.r, c.g, c.b, alpha);
}

// The stage select's two cursors (StageCursors.h): each player's pointer in their colour with
// their name, and the stage each proposes framed in their colour. Positions are the match block's
// (published by the frame hook), mapped onto the picture with the layout's probed map.
void DrawStageCursors(ImDrawList* dl, const StageCursors::View& v, float unit)
{
  const StageCursors::Layout* layout = StageCursors::LayoutFor(v.game);
  if (!layout || !g_presenter)
    return;
  const MathUtil::Rectangle<int>& r = g_presenter->GetTargetRectangle();
  const float left = static_cast<float>(r.left), top = static_cast<float>(r.top);
  const float w = static_cast<float>(r.GetWidth()), h = static_cast<float>(r.GetHeight());
  if (!(w >= 1 && h >= 1))
    return;
  const auto at = [&](float x, float y) {
    return ImVec2(left + w * (layout->u0 + layout->du * x),
                  top + h * (layout->v0 + layout->dv * y));
  };
  const auto at16 = [&](int x16, int y16) {
    return at(static_cast<float>(x16) / 16.0f, static_cast<float>(y16) / 16.0f);
  };
  const float label = 2.6f * unit;
  // The proposals: a frame round the tile in the proposer's colour, a second one inside it when
  // both propose the same stage, and the name on a tab at its top.
  for (int port = 0; port < 2; ++port)
  {
    const StageCursors::View::Player& p = v.players[port];
    if (!p.present || p.proposal < 0)
      continue;
    const StageCursors::Tile* tile = StageCursors::TileOf(*layout, p.proposal, v.page);
    if (!tile)
      continue;
    const bool both = v.players[1 - port].present && v.players[1 - port].proposal == p.proposal;
    const ImVec2 c0 = at16(tile->rect.x0, tile->rect.y1);
    const ImVec2 c1 = at16(tile->rect.x1, tile->rect.y0);
    const float inset = both && port == 1 ? 0.9f * unit : 0.0f;
    const ImVec2 a(std::min(c0.x, c1.x) + inset, std::min(c0.y, c1.y) + inset);
    const ImVec2 b(std::max(c0.x, c1.x) - inset, std::max(c0.y, c1.y) - inset);
    dl->AddRect(a, b, Rgba(10, 10, 16, 0.85f), 0.6f * unit, 0, 0.9f * unit);
    dl->AddRect(a, b, PortRgba(port, 1), 0.6f * unit, 0, 0.55f * unit);
    const std::string tag = both ? (port == 0 ? p.name + " + " + v.players[1].name : "") :
                                   p.name;
    if (tag.empty())
      continue;
    const ImVec2 s = TextSize(label, tag);
    const ImVec2 t(port == 0 ? a.x : b.x - s.x - unit, a.y - s.y - 0.4f * unit);
    dl->AddRectFilled(ImVec2(t.x - 0.4f * unit, t.y - 0.1f * unit),
                      ImVec2(t.x + s.x + 0.4f * unit, t.y + s.y + 0.1f * unit),
                      both ? Rgba(250, 210, 60, 0.95f) : PortRgba(port, 0.95f), 0.4f * unit);
    dl->AddText(ImGui::GetFont(), TextPx(label), t,
                both ? Rgba(20, 20, 20, 1) : Rgba(255, 255, 255, 1), tag.c_str());
  }
  // The cursors: a crosshair like the game's own (a light disc, a ring, four ticks) in the
  // player's colour, opaque and a little larger than the game's, so the game's own crosshair (which
  // sits on the cursor that moved last) never shows; a gold ring outside for the player whose turn
  // it is; a tab with the port and the name, port 1's to the left and port 2's to the right so two
  // cursors on one tile keep both.
  const float radius = 1.7f * std::abs(layout->du) * w;
  for (int port = 0; port < 2; ++port)
  {
    const StageCursors::View::Player& p = v.players[port];
    if (!p.present)
      continue;
    const ImVec2 c = at16(p.x, p.y);
    const float thick = std::max(2.0f, 0.24f * radius);
    if (v.turn == port)
    {
      dl->AddCircle(c, radius + 0.55f * thick, Rgba(10, 10, 16, 0.9f), 40, 0.9f * thick);
      dl->AddCircle(c, radius + 0.55f * thick, Rgba(250, 210, 60, 1), 40, 0.6f * thick);
    }
    dl->AddCircleFilled(c, radius, Rgba(10, 10, 16, 1), 40);
    dl->AddCircleFilled(c, radius - 1.5f, Rgba(246, 246, 246, 1), 40);
    dl->AddCircle(c, radius * 0.72f, PortRgba(port, 1), 40, thick);
    for (const ImVec2 d : {ImVec2(1, 0), ImVec2(-1, 0), ImVec2(0, 1), ImVec2(0, -1)})
    {
      dl->AddLine(ImVec2(c.x + d.x * radius * 0.86f, c.y + d.y * radius * 0.86f),
                  ImVec2(c.x + d.x * radius * 0.30f, c.y + d.y * radius * 0.30f),
                  PortRgba(port, 1), 0.8f * thick);
    }
    const std::string tag = fmt::format("P{} {}", port + 1, p.name);
    const ImVec2 ts = TextSize(label, tag);
    const float ty = c.y + radius * 0.75f;
    const float tx = port == 0 ? c.x - radius * 0.75f - ts.x : c.x + radius * 0.75f;
    dl->AddRectFilled(ImVec2(tx - 0.4f * unit, ty - 0.1f * unit),
                      ImVec2(tx + ts.x + 0.4f * unit, ty + ts.y + 0.1f * unit),
                      PortRgba(port, 0.95f), 0.4f * unit);
    dl->AddText(ImGui::GetFont(), TextPx(label), ImVec2(tx, ty), Rgba(255, 255, 255, 1),
                tag.c_str());
  }
}

// The game's picture in the window (the presenter's target rectangle: the window can letterbox or
// pillarbox it), the whole display when the presenter has none.
void PictureRect(ImVec2* pic, ImVec2* pic_size)
{
  *pic = ImVec2(0, 0);
  *pic_size = ImGui::GetIO().DisplaySize;
  if (g_presenter)
  {
    const MathUtil::Rectangle<int>& r = g_presenter->GetTargetRectangle();
    if (r.GetWidth() > 0 && r.GetHeight() > 0)
    {
      *pic = ImVec2(static_cast<float>(r.left), static_cast<float>(r.top));
      *pic_size = ImVec2(static_cast<float>(r.GetWidth()), static_cast<float>(r.GetHeight()));
    }
  }
}

// ORCA_UX_KIT_DEMO=1: the kit's primitives over the game's picture (Kit.h), for design checks.
void DrawKitDemo()
{
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  if (OverlayUnit(screen.y) <= 0 || !(screen.x >= 1))
    return;
  ImVec2 pic, pic_size;
  PictureRect(&pic, &pic_size);
  Kit::DrawDemo(ImGui::GetForegroundDrawList(), pic, pic_size, OverlayUnit(pic_size.y),
                ImGui::GetTime());
}

// A line's trailing " · m:ss" (the turn's timer), taken off it: the seconds, or -1 with none.
int TakeTimer(std::string* line)
{
  static const std::string sep = " \u00b7 ";
  const std::size_t at = line->rfind(sep);
  if (at == std::string::npos)
    return -1;
  const std::string tail = line->substr(at + sep.size());
  const std::size_t colon = tail.find(':');
  if (colon == std::string::npos || colon == 0 || tail.size() != colon + 3)
    return -1;
  int minutes = 0;
  for (std::size_t i = 0; i < colon; ++i)
  {
    if (tail[i] < '0' || tail[i] > '9')
      return -1;
    minutes = minutes * 10 + (tail[i] - '0');
  }
  if (tail[colon + 1] < '0' || tail[colon + 1] > '5' || tail[colon + 2] < '0' || tail[colon + 2] > '9')
    return -1;
  line->resize(at);
  return minutes * 60 + (tail[colon + 1] - '0') * 10 + (tail[colon + 2] - '0');
}

// ORCA_UX_KIT_DEMO=2: the overlay's own elements, each drawn with the kit (Kit.h), for design
// checks: what the convert stage makes of them. The same data as Draw's, nothing else.
void DrawKitPreview(const OverlayModel::Frame& f, std::string search, std::string search_note,
                    const std::array<std::string, 4>& labels, const std::array<bool, 4>& locked,
                    std::string rules, std::string rules_note, const StageCursors::View& cursors)
{
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  if (OverlayUnit(screen.y) <= 0 || !(screen.x >= 1))
    return;
  ImVec2 pic, pic_size;
  PictureRect(&pic, &pic_size);
  const float unit = OverlayUnit(pic_size.y);
  if (unit <= 0)
    return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const double now_s = ImGui::GetTime();
  // The stage select: each player's pointer, each proposal framed.
  if (const StageCursors::Layout* layout = StageCursors::LayoutFor(cursors.game); cursors.on && layout)
  {
    const auto at16 = [&](int x16, int y16) {
      return ImVec2(pic.x + pic_size.x * (layout->u0 + layout->du * static_cast<float>(x16) / 16.0f),
                    pic.y + pic_size.y * (layout->v0 + layout->dv * static_cast<float>(y16) / 16.0f));
    };
    for (int port = 0; port < 2; ++port)
    {
      const StageCursors::View::Player& p = cursors.players[port];
      if (!p.present || p.proposal < 0)
        continue;
      const StageCursors::Tile* tile = StageCursors::TileOf(*layout, p.proposal, cursors.page);
      if (!tile)
        continue;
      const bool both = cursors.players[1 - port].present &&
                        cursors.players[1 - port].proposal == p.proposal;
      if (both && port == 1)
        continue;
      const ImVec2 c0 = at16(tile->rect.x0, tile->rect.y1), c1 = at16(tile->rect.x1, tile->rect.y0);
      Kit::Frame(dl, ImVec2(std::min(c0.x, c1.x), std::min(c0.y, c1.y)),
                 ImVec2(std::max(c0.x, c1.x), std::max(c0.y, c1.y)), unit,
                 both ? Kit::Tone::Gold : Kit::PortTone(port),
                 both ? p.name + " + " + cursors.players[1].name : p.name, port == 0);
    }
    const float radius = 1.7f * std::abs(layout->du) * pic_size.x;
    for (int port = 0; port < 2; ++port)
    {
      const StageCursors::View::Player& p = cursors.players[port];
      if (p.present)
      {
        Kit::Pointer(dl, at16(p.x, p.y), radius, unit, port,
                     fmt::format("P{} {}", port + 1, p.name), cursors.turn == port, port == 0,
                     now_s);
      }
    }
  }
  // Top left: the connection's lamp, the ping and the meter beside it, the toasts and the chat on
  // plates under them.
  const float margin = 2.5f * unit;
  float y = margin;
  if (f.show_signal)
  {
    const float r = 3.0f * unit;
    Kit::Connection(dl, ImVec2(margin + r, margin + r), r, unit,
                    f.signal == Signal::Good ? 3 : f.signal == Signal::Ok ? 2 : 1);
    float ty = margin + r - 1.5f * unit;
    if (!f.stats.empty())
      ty = Kit::Label(dl, ImVec2(margin + 2 * r + unit, ty), unit, f.stats, 2.6f).max.y;
    if (!f.perf.empty())
      Kit::Label(dl, ImVec2(margin + 2 * r + unit, ty), unit, f.perf, 2.6f);
    y = margin + 2 * r + 1.5f * unit;
  }
  else if (!f.perf.empty())
  {
    y = Kit::Label(dl, ImVec2(margin, y), unit, f.perf, 2.6f).max.y + unit;
  }
  if (!f.toasts.empty() || !f.chats.empty())
    y = std::max(y, OSD::MessagesBottom() + unit);
  for (const OverlayModel::Toast& t : f.toasts)
  {
    y = Kit::LabelPlate(dl, ImVec2(margin, y), unit, t.text, 2.7f, Kit::Align::Left,
                        Kit::Tone::Panel, t.alpha)
            .max.y +
        0.9f * unit;
  }
  for (const OverlayModel::ChatToast& c : f.chats)
  {
    const Kit::TextLook look = Kit::LabelLook(unit, 2.7f);
    for (const std::string& line : Kit::Wrap(look, c.name + ": " + c.text, 0.4f * pic_size.x))
    {
      y = Kit::LabelPlate(dl, ImVec2(margin, y), unit, line, 2.7f, Kit::Align::Left,
                          Kit::Tone::Panel, c.alpha)
              .max.y +
          0.5f * unit;
    }
    y += 0.4f * unit;
  }
  // Top centre: the search's or the set's lines on a banner, the turn's timer beside it.
  std::vector<std::string> lines;
  for (std::string* l : {&search, &search_note, &rules, &rules_note})
  {
    if (!l->empty())
      lines.push_back(*l);
  }
  int seconds = -1;
  for (std::string& l : lines)
  {
    if (seconds < 0)
      seconds = TakeTimer(&l);
  }
  float banner_top = 2.0f * unit;
  for (std::size_t i = 0; i < lines.size(); i += 2)
  {
    const std::string& title = lines[i];
    const std::string note = i + 1 < lines.size() ? lines[i + 1] : std::string();
    const float h = note.empty() ? 6.5f * unit : 9.0f * unit;
    const float w = std::max(Kit::Measure(Kit::LabelLook(unit, h * 0.44f / unit), title).x,
                             Kit::Measure(Kit::LabelLook(unit, h * 0.3f / unit), note).x) +
                    10 * unit;
    const float cx = pic.x + pic_size.x / 2;
    const Kit::Box b = Kit::Banner(dl, cx - w / 2, cx + w / 2, banner_top, h, unit,
                                   Kit::Tone::Panel, title, note);
    if (i == 0 && seconds >= 0)
      Kit::Timer(dl, ImVec2(b.max.x + 6.5f * unit, (b.min.y + b.max.y) / 2), unit, seconds, now_s);
    banner_top = b.max.y + 1.2f * unit;
  }
  // The character select's panels: each name on a plate in the port's colour, LOCKED IN under it.
  for (int port = 0; port < 4; ++port)
  {
    if (labels[port].empty())
      continue;
    const float centre = pic.x + pic_size.x * PanelCentre(port);
    Kit::LabelPlate(dl, ImVec2(centre, pic.y + pic_size.y * 0.575f), unit, labels[port], 2.8f,
                    Kit::Align::Centre, Kit::PortTone(port));
    if (locked[port])
    {
      Kit::Badge(dl, ImVec2(centre, pic.y + pic_size.y * LOCKED_BADGE_Y), unit, "LOCKED IN",
                 Kit::PortTone(port), 3.2f);
    }
  }
  if (!f.waiting.empty())
    Kit::Title(dl, ImVec2(pic.x + pic_size.x / 2, pic.y + pic_size.y * 0.18f), unit, f.waiting, 5.0f,
               Kit::Align::Centre);
}

void Draw()
{
  Kit::BeginFrame();
  const double now = NowMs();
  s_overlay.Update(s_demo ? DemoLink(now) : ViewOf(Events::GetLinkStats()), now);
  OverlayModel::Frame f = s_overlay.View(now, s_stats.load(std::memory_order_relaxed),
                                         s_perf.load(std::memory_order_relaxed));
  // The set's end and the opponent's disconnect (SetEnd.h): its notice says more than "Waiting
  // for <name>…", which goes meanwhile.
  SetEnd::DemoFrame(now);
  const SetEnd::View set_end = SetEnd::Current().At(now);
  if (set_end.hides_waiting)
    f.waiting.clear();
  // The matchmaking search (UX/OnlineMenu.h), while the player waits on the character select; the
  // queue's character select's lines (UX/Queue.h) in their place when it has any.
  auto [search, search_note] = Search::Lines();
  std::array<std::string, 4> labels;
  std::array<bool, 4> locked{};
  {
    std::lock_guard lk(s_queue_lock);
    if (!s_queue_line.empty() || !s_queue_note.empty())
    {
      search = s_queue_line;
      search_note = s_queue_note;
    }
    labels = s_queue_labels;
    locked = s_queue_locked;
  }
  const bool any_label =
      std::any_of(labels.begin(), labels.end(), [](const std::string& l) { return !l.empty(); });
  // A ranked set's lines, top centre: the stage flow's (Brawl's through SetRulesLines, Project+'s
  // RankedPPlus::CurrentLines), else the set's score on the menus (UX/RankedSet.h); under it the
  // turn (the stage flow's, else the character order's).
  std::string rules, rules_note, rule_line;
  {
    std::lock_guard lk(s_rules_lock);
    rules = s_rules_line;
    rules_note = s_rules_note;
  }
  {
    std::lock_guard lk(s_rule_line_lock);
    rule_line = s_rule_line;
  }
  if (rules.empty() && rules_note.empty())
  {
    const RankedPPlus::Lines ranked = RankedPPlus::CurrentLines();
    rules = ranked.set;
    rules_note = ranked.turn;
  }
  if (rules.empty())
    rules = RankedSet::CurrentScoreLine();
  if (rules_note.empty())
    rules_note = rule_line;
  const StageCursors::View cursors = StageCursors::Current();
  // The kit's demo or preview in place of everything else.
  if (Kit::DemoMode() == 1)
  {
    DrawKitDemo();
    return;
  }
  if (Kit::DemoMode() == 2)
  {
    DrawKitPreview(f, search, search_note, labels, locked, rules, rules_note, cursors);
    return;
  }
  if (!f.show_signal && f.waiting.empty() && f.toasts.empty() && f.chats.empty() &&
      f.stats.empty() && f.perf.empty() && search.empty() && search_note.empty() &&
      rules.empty() && rules_note.empty() && !any_label && !cursors.on && set_end.Empty())
  {
    return;
  }
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  const float unit = OverlayUnit(screen.y);
  // No picture to draw on (no backbuffer yet, a window the app squeezed to a sliver): nothing.
  if (unit <= 0 || !(screen.x >= 1))
    return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float margin = 3 * unit;
  const float text_size = 3.0f * unit;
  // The stage select's two cursors first: every line of text goes over them.
  if (cursors.on)
    DrawStageCursors(dl, cursors, unit);
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
  // Dolphin's own messages (OSD rows such as "Screenshot saved...", drawn at the top left just
  // before this) push the toasts and the chat under them down: never drawn over them.
  if (!f.toasts.empty() || !f.chats.empty())
    y = std::max(y, OSD::MessagesBottom() + unit);
  for (const OverlayModel::Toast& t : f.toasts)
  {
    OutlinedText(dl, text_size, ImVec2(margin, y), t.alpha, t.text);
    y += TextSize(text_size, t.text).y + unit;
  }
  if (any_label)
  {
    // Each player's name over their panel at the bottom of the character select, and a locked-in
    // player's badge across the panel's picture, in the game's picture (the window can letterbox
    // or pillarbox it): both games' widescreen panels sit at PanelCentre's fractions of its width,
    // their tops at about 60% of its height (measured on 1280x720 screenshots, 2026-10-04).
    ImVec2 pic(0, 0), pic_size = screen;
    if (g_presenter)
    {
      const MathUtil::Rectangle<int>& r = g_presenter->GetTargetRectangle();
      if (r.GetWidth() > 0 && r.GetHeight() > 0)
      {
        pic = ImVec2(static_cast<float>(r.left), static_cast<float>(r.top));
        pic_size = ImVec2(static_cast<float>(r.GetWidth()), static_cast<float>(r.GetHeight()));
      }
    }
    const float size = 3.2f * unit;
    for (int port = 0; port < 4; ++port)
    {
      if (labels[port].empty())
        continue;
      const ImVec2 s = TextSize(size, labels[port]);
      const float centre = pic.x + pic_size.x * PanelCentre(port);
      OutlinedText(dl, size, ImVec2(centre - s.x / 2, pic.y + pic_size.y * 0.585f), 1,
                   labels[port]);
      if (locked[port])
      {
        LockedBadge(dl, unit, ImVec2(centre, pic.y + pic_size.y * LOCKED_BADGE_Y),
                    PortColourOf(port));
      }
    }
  }
  // How far down the top-centre lines reach, and how far left the widest goes (the chat keeps
  // clear of them).
  float centre_bottom = 0;
  float centre_left = screen.x;
  const auto centred = [&](float size, float line_y, const std::string& text, Signal level) {
    const ImVec2 s = TextSize(size, text);
    const float x = (screen.x - s.x) / 2;
    OutlinedText(dl, size, ImVec2(x, line_y), 1, text, level);
    centre_bottom = std::max(centre_bottom, line_y + s.y);
    centre_left = std::min(centre_left, x);
    return s;
  };
  if (!search.empty())
  {
    // Top centre, clear of the circle and the meter in the top-left corner.
    const float size = 3.0f * unit;
    float line_y = margin;
    const ImVec2 s = centred(size, line_y, search, Signal::Good);
    if (!search_note.empty())
    {
      line_y += s.y + 0.5f * unit;
      centred(size, line_y, search_note, Signal::Ok);
    }
  }
  if (!rules.empty() || !rules_note.empty())
  {
    // Top centre too (the search lines are gone once the opponent is in).
    const float size = 3.0f * unit;
    float line_y = margin;
    if (!search.empty())
      line_y += 2 * (TextSize(size, search).y + 0.5f * unit);
    if (!rules.empty())
    {
      const ImVec2 s = centred(size, line_y, rules, Signal::Good);
      line_y += s.y + 0.5f * unit;
    }
    if (!rules_note.empty())
      centred(size, line_y, rules_note, Signal::Ok);
  }
  if (!f.chats.empty())
  {
    // Under the toasts, wrapped to under half the width; below the top-centre lines where they
    // would meet.
    const float wrap = 0.45f * screen.x;
    if (centre_bottom > 0 && centre_left < margin + wrap)
      y = std::max(y, centre_bottom + 1.5f * unit);
    for (const OverlayModel::ChatToast& c : f.chats)
    {
      if (y >= screen.y - margin)
        break;
      y += ChatLine(dl, text_size, ImVec2(margin, y), wrap, c) + unit;
    }
  }
  // Over everything else, in the game's picture (the window can letterbox or pillarbox it).
  if (!set_end.Empty())
  {
    ImVec2 pic(0, 0), pic_size = screen;
    if (g_presenter)
    {
      const MathUtil::Rectangle<int>& r = g_presenter->GetTargetRectangle();
      if (r.GetWidth() > 0 && r.GetHeight() > 0)
      {
        pic = ImVec2(static_cast<float>(r.left), static_cast<float>(r.top));
        pic_size = ImVec2(static_cast<float>(r.GetWidth()), static_cast<float>(r.GetHeight()));
      }
    }
    SetEnd::Draw(dl, unit, pic.x, pic.y, pic_size.x, pic_size.y, set_end);
  }
}
}  // namespace

OverlayModel& Overlay()
{
  return s_overlay;
}

PortColour PortColourOf(int port)
{
  // Brawl's and Project+'s port colours (the panels, the tokens).
  switch (port)
  {
  case 0:
    return {224, 44, 44};
  case 1:
    return {40, 104, 236};
  case 2:
    return {232, 180, 24};
  default:
    return {44, 168, 64};
  }
}

float OverlayUnit(float display_height)
{
  // !(>=) also turns a NaN height away.
  if (!(display_height >= OVERLAY_MIN_HEIGHT))
    return 0;
  return display_height / 100;
}

void SetRulesLines(std::string line, std::string note)
{
  std::lock_guard lk(s_rules_lock);
  s_rules_line = std::move(line);
  s_rules_note = std::move(note);
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

void ShowToast(std::string text)
{
  s_overlay.AddToast(std::move(text), NowMs());
}

void SetQueueLines(std::string line, std::string note, std::array<std::string, 4> labels,
                   std::array<bool, 4> locked)
{
  std::lock_guard lk(s_queue_lock);
  s_queue_line = std::move(line);
  s_queue_note = std::move(note);
  s_queue_labels = std::move(labels);
  s_queue_locked = locked;
}

void ShowChat(std::string name, std::string text)
{
  s_overlay.AddChat(std::move(name), std::move(text), NowMs());
}

void SetRuleLine(std::string text)
{
  std::lock_guard lk(s_rule_line_lock);
  s_rule_line = std::move(text);
}

void ShutdownOverlay()
{
  OSD::SetHostOverlay({});
  Kit::Shutdown();
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
