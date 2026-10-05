// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/HookableEvent.h"
#include "Core/Core.h"
#include "Core/Orca/UX/CharOrder.h"
#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/NativeText.h"
#include "Core/Orca/UX/OnlineMenu.h"
#include "Core/Orca/UX/Queue.h"
#include "Core/Orca/UX/RankedPPlus.h"
#include "Core/Orca/UX/RankedSet.h"
#include "Core/Orca/UX/Relabel.h"
#include "Core/Orca/UX/SetEnd.h"
#include "Core/Orca/UX/StageCursors.h"
#include "Core/Orca/UX/YgCard.h"
#include "Core/Orca/UX/YgOrbDraw.h"
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
  // A toast and two chat lines each round, the second long enough to wrap.
  static int s_chat_round = -1;
  const int round = static_cast<int>((now - s_demo_start) / 12000);
  if (round != s_chat_round && t >= 1500)
  {
    s_chat_round = round;
    s_overlay.AddToast("ada joined on port 2", now);
    s_overlay.AddChat("ada", "gl hf!", now);
    s_overlay.AddChat("ada", "one more after this? I want a rematch on Battlefield", now);
  }
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

// The frame meter's colours: white while the game keeps its pace, then the lamp's yellow and red.
ImU32 LevelColour(Signal level, float alpha)
{
  return level == Signal::Bad ? Rgba(255, 72, 64, alpha) :
         level == Signal::Ok  ? Rgba(255, 208, 64, alpha) :
                                Rgba(255, 255, 255, alpha);
}

// The frame meter's graph on a panel plate: a bar per shown frame of the last two seconds, newest
// on the right, up to 50 ms tall, with a line at one frame period.
void FrameGraph(ImDrawList* dl, ImVec2 pos, ImVec2 size, float unit,
                const std::vector<float>& frames)
{
  constexpr float TOP_MS = 50;
  constexpr float SLOTS = OverlayModel::PERF_GRAPH_MS * 60 / 1000;
  Kit::Plate(dl, pos, ImVec2(pos.x + size.x, pos.y + size.y), unit, Kit::Tone::Panel, 0.9f);
  // The bars inside the plate's rim.
  const float pad = 0.9f * unit;
  pos = ImVec2(pos.x + pad, pos.y + pad);
  size = ImVec2(size.x - 2 * pad, size.y - 2 * pad);
  dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), Rgba(0, 0, 0, 0.45f),
                    0.4f * unit);
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
                      level == Signal::Good ? Rgba(170, 220, 255, 0.85f) : LevelColour(level, 1));
    x -= slot;
  }
  const float period_y = bottom - 1000.0f / 60 / TOP_MS * size.y;
  dl->AddLine(ImVec2(pos.x, period_y), ImVec2(pos.x + size.x, period_y),
              Rgba(255, 255, 255, 0.4f), std::max(1.0f, size.y / 40));
}

// The centre of port `port`'s panel on the character select, as a fraction of the game's picture's
// width (both games, widescreen: ports 1 and 2 at 0.236 and 0.413 of a 1280x720 picture, 0.177
// apart).
float PanelCentre(int port)
{
  return 0.236f + 0.177f * static_cast<float>(port);
}

// Where the name tab sits over a panel (its top), as a fraction of the picture's height.
constexpr float NAME_TAB_Y = 0.575f;

// Port `port`'s portrait on the character select in a picture at `pic`, `pic_size` (design F's
// frames and LOCKED IN ribbon go round it), measured on 1280x720 screenshots of both games
// (2026-10-04): 200 pixels wide round the panel's centre; Brawl's from under the READY TO FIGHT
// band (y 466) to its name plate (622), Project+'s from 432 to 628.
Kit::Box PortraitRect(int port, ImVec2 pic, ImVec2 pic_size)
{
  const bool pplus = Kit::CurrentLook() == Kit::Look::ProjectPlus;
  const float cx = pic.x + pic_size.x * PanelCentre(port);
  const float half = pic_size.x * 0.078f;
  return {ImVec2(cx - half, pic.y + pic_size.y * (pplus ? 0.6f : 0.647f)),
          ImVec2(cx + half, pic.y + pic_size.y * (pplus ? 0.872f : 0.864f))};
}

// The set's dots' middle, from the picture's top: in both games' dark top band on the character
// and the stage selects.
constexpr float DOTS_Y = 1.9f;  // overlay units
// Where the top centre's banners start: under the dots when they show.
constexpr float TOP_FIRST = 1.6f, TOP_UNDER_DOTS = 3.9f;  // overlay units

// Design F's timers in the game's lettering (Kit::GameTimer), measured on 1280x720 screenshots of
// both games (2026-10-05). READY TO FIGHT!'s: centred on the band's right end, past its words
// (Brawl x 1186 y 441, Project+ x 1180 y 408). STAGE SELECT's: its left edge just past the
// texture's right end (Brawl x 475, y 673; Project+ x 530, y 46; the labels' art ends there).
struct GameTimerPlace
{
  float x, y, size;
};
constexpr GameTimerPlace BAND_TIMER_BRAWL{0.927f, 0.6125f, 5.2f};
constexpr GameTimerPlace BAND_TIMER_PPLUS{0.922f, 0.567f, 5.2f};
constexpr GameTimerPlace LINE_TIMER_BRAWL{0.371f, 0.935f, 4.4f};
constexpr GameTimerPlace LINE_TIMER_PPLUS{0.414f, 0.064f, 5.4f};
// The control guide's right end and middle: the games' own corner for their button legends.
constexpr float GUIDE_RIGHT = 0.975f, GUIDE_Y = 0.969f;

// A name in capitals as the game's own plates write them (ASCII letters only).
std::string Upper(std::string_view name)
{
  std::string out(name);
  for (char& c : out)
  {
    if (c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
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

// The stage select's two cursors (StageCursors.h), in the kit: each player's pointer in their
// colour with a tab naming them (pulsing on their turn), and the stage each proposes
// framed in their colour with their name on a tab; both on one stage, one gold frame with both
// names. Positions are the match block's (published by the frame hook), mapped onto the picture
// with the layout's probed map.
void DrawStageCursors(ImDrawList* dl, const StageCursors::View& v, ImVec2 pic, ImVec2 pic_size,
                      float unit, double now_s)
{
  const StageCursors::Layout* layout = StageCursors::LayoutFor(v.game);
  if (!layout)
    return;
  const auto at16 = [&](int x16, int y16) {
    return ImVec2(
        pic.x + pic_size.x * (layout->u0 + layout->du * static_cast<float>(x16) / 16.0f),
        pic.y + pic_size.y * (layout->v0 + layout->dv * static_cast<float>(y16) / 16.0f));
  };
  for (int port = 0; port < 2; ++port)
  {
    const StageCursors::View::Player& p = v.players[port];
    if (!p.present || p.proposal < 0)
      continue;
    const StageCursors::Tile* tile = StageCursors::TileOf(*layout, p.proposal, v.page);
    if (!tile)
      continue;
    const bool both = v.players[1 - port].present && v.players[1 - port].proposal == p.proposal;
    if (both && port == 1)
      continue;
    const ImVec2 c0 = at16(tile->rect.x0, tile->rect.y1), c1 = at16(tile->rect.x1, tile->rect.y0);
    Kit::Frame(dl, ImVec2(std::min(c0.x, c1.x), std::min(c0.y, c1.y)),
               ImVec2(std::max(c0.x, c1.x), std::max(c0.y, c1.y)), unit,
               both ? Kit::Tone::Gold : Kit::PortTone(port),
               both ? Upper(p.name) + " + " + Upper(v.players[1].name) : Upper(p.name), port == 0);
  }
  // Opaque and a little larger than the game's own crosshair (which sits on the cursor that moved
  // last), so it never shows; port 1's tab to the left and port 2's to the right, so two cursors on
  // one tile keep both.
  const float radius = 1.7f * std::abs(layout->du) * pic_size.x;
  for (int port = 0; port < 2; ++port)
  {
    const StageCursors::View::Player& p = v.players[port];
    if (p.present)
    {
      Kit::Pointer(dl, at16(p.x, p.y), radius, unit, port,
                   fmt::format("P{} {}", port + 1, Upper(p.name)), v.turn == port, port == 0, now_s);
    }
  }
}

// ORCA_UX_KIT_DEMO=1: the kit's primitives over the game's picture (Kit.h), for design checks.
void DrawKitDemo()
{
  ImVec2 pic, pic_size;
  PictureRect(&pic, &pic_size);
  const float unit = OverlayUnit(pic_size.y);
  if (unit <= 0 || !(pic_size.x >= 1))
    return;
  Kit::DrawDemo(ImGui::GetForegroundDrawList(), pic, pic_size, unit, ImGui::GetTime());
}

// A line's trailing " · m:ss" (the turn's timer), taken off it: the seconds, or -1 with none.
int TakeTimer(std::string* line)
{
  static const std::string sep = " · ";
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

// Everything the overlay shows this frame, gathered from the model and the UX's tracks.
struct Scene
{
  OverlayModel::Frame f;
  SetEnd::View set_end;
  // Top centre: the search's or the queue's lines, then the set's (the stage flow's, the score,
  // the turn); `hint` this machine's own (Hold Z, B to change) as the control guide, bottom right.
  std::string search, search_note, rules, rules_note, hint;
  // Design F (Relabel.h): the turn's timer at READY TO FIGHT!'s right end and beside STAGE
  // SELECT's label when those say Orca's words (-1: not there).
  int band_seconds = -1;
  int line_seconds = -1;
  bool legend = false;  // Project+'s stage select legend says X, A and B (Relabel.h)
  std::array<std::string, 4> labels;
  std::array<bool, 4> locked{};
  StageCursors::View cursors;
  // Design F: whose pick it is in the character order (-1 none), the set's dots, and the queue's
  // own character select searching (a ring on the empty seat).
  int picker = -1;
  RankedSet::Dots dots;
  bool searching = false;

  bool Empty() const
  {
    return !f.show_signal && f.waiting.empty() && f.toasts.empty() && f.chats.empty() &&
           f.stats.empty() && f.perf.empty() && search.empty() && search_note.empty() &&
           rules.empty() && rules_note.empty() && hint.empty() &&
           std::all_of(labels.begin(), labels.end(),
                       [](const std::string& l) { return l.empty(); }) &&
           std::none_of(locked.begin(), locked.end(), [](bool l) { return l; }) && !cursors.on &&
           picker < 0 && !dots.on && !searching && band_seconds < 0 && line_seconds < 0 &&
           set_end.Empty();
  }
};

Scene Gather(double now)
{
  Scene s;
  s_overlay.Update(s_demo ? DemoLink(now) : ViewOf(Events::GetLinkStats()), now);
  s.f = s_overlay.View(now, s_stats.load(std::memory_order_relaxed),
                       s_perf.load(std::memory_order_relaxed));
  // The set's end and the opponent's disconnect (SetEnd.h): its notice says more than "Waiting
  // for <name>…", which goes meanwhile.
  s.set_end = SetEnd::Current().At(SetEnd::DemoFrame(now));
  if (s.set_end.hides_waiting)
    s.f.waiting.clear();
  // The matchmaking search (UX/OnlineMenu.h), while the player waits on the character select; the
  // queue's character select's lines (UX/Queue.h) in their place when it has any.
  std::tie(s.search, s.search_note) = Search::Lines();
  {
    std::lock_guard lk(s_queue_lock);
    if (!s_queue_line.empty() || !s_queue_note.empty())
    {
      s.search = s_queue_line;
      s.search_note = s_queue_note;
    }
    s.labels = s_queue_labels;
    s.locked = s_queue_locked;
  }
  // Design F (Relabel.h): with READY TO FIGHT! up and saying Orca's words, the turn's timer is
  // the band's (the lines lose theirs; the rules bar already has, NativeText.h).
  const Relabel::Shown words = Relabel::Current();
  const bool band_clock = words.band != Relabel::Band::Game && words.band_up;
  const auto take = [](int* seconds, std::string* line) {
    const int t = TakeTimer(line);
    if (*seconds < 0)
      *seconds = t;
  };
  if (band_clock)
  {
    take(&s.band_seconds, &s.search);
    take(&s.band_seconds, &s.search_note);
  }
  // What the game's own text boxes already say (NativeText.h): a plate that names its player
  // needs no tab over it (its LOCKED IN ribbon stays), and a rules bar that says the queue's line
  // takes the place of the top lines; this machine's own hints (Hold Z, B to change) stay, in the
  // control guide.
  const u8 native = NativeText::Shown();
  for (int port = 0; port < 4; ++port)
  {
    if (native & (1 << port))
      s.labels[port].clear();
  }
  if (native & NativeText::SHOWN_RULES)
  {
    // On the queue's own character select the bar says it all (it is this player's alone).
    s.hint = Queue::OnSoloCss() ? std::string() : std::move(s.search_note);
    s.search.clear();
    s.search_note.clear();
  }
  // A ranked set's lines, top centre: the stage flow's (Brawl's through SetRulesLines, Project+'s
  // RankedPPlus::CurrentLines), else the set's score on the menus (UX/RankedSet.h); under it the
  // turn (the stage flow's, else the character order's).
  std::string rule_line;
  {
    std::lock_guard lk(s_rules_lock);
    s.rules = s_rules_line;
    s.rules_note = s_rules_note;
  }
  {
    std::lock_guard lk(s_rule_line_lock);
    rule_line = s_rule_line;
  }
  if (s.rules.empty() && s.rules_note.empty())
  {
    const RankedPPlus::Lines ranked = RankedPPlus::CurrentLines();
    s.rules = ranked.set;
    s.rules_note = ranked.turn;
  }
  if (s.rules.empty())
    s.rules = RankedSet::CurrentScoreLine();
  if (s.rules_note.empty())
    s.rules_note = rule_line;
  if (band_clock)
  {
    take(&s.band_seconds, &s.rules);
    take(&s.band_seconds, &s.rules_note);
  }
  // With STAGE SELECT saying whose turn it is, the stage select's lines go: its timer beside the
  // label, the set's score in the dots, the turn's buttons in the guide.
  if (words.line != Relabel::LINE_GAME)
  {
    take(&s.line_seconds, &s.rules);
    take(&s.line_seconds, &s.rules_note);
    const auto join = [](std::string* to, const std::string& more) {
      if (!more.empty())
        *to = to->empty() ? more : *to + " \u00b7 " + more;
    };
    join(&s.hint, s.rules_note);
    // The label says P1 PICKS; Brawl's Meta Knight clause is why it may be any stage.
    if (s.rules.find("MK clause") != std::string::npos)
      join(&s.hint, "Meta Knight clause: any legal stage");
    s.rules.clear();
    s.rules_note.clear();
  }
  s.legend = words.legend;
  // The rules bar says the set's score and the character order's turn too; the queue's own
  // character select has no set yet (a ranked header is there already).
  if ((native & NativeText::SHOWN_RULES) || Queue::OnSoloCss())
  {
    s.rules.clear();
    s.rules_note.clear();
  }
  s.cursors = StageCursors::Current();
  // The character and the stage selects only: the order's picker, the set's dots (not on the
  // queue's own character select: no opponent, no set yet, though a ranked header is there).
  if (Queue::OnQueueCss())
    s.picker = CharOrder::CurrentPicker();
  if ((Queue::OnQueueCss() && !Queue::OnSoloCss()) || s.cursors.on)
    s.dots = RankedSet::CurrentDots();
  s.searching = Queue::OnSoloCss() && Queue::SoloReady();
  return s;
}

// Top left: the link's lamp with the ping and the frame meter beside it (the meter alone when
// solo), the meter's graph under them. Returns the bottom.
float DrawCorner(ImDrawList* dl, const OverlayModel::Frame& f, ImVec2 at, float unit)
{
  constexpr float SIZE = 2.6f;
  const float line_h = Kit::LabelLook(unit, SIZE).px;
  float y = at.y;
  if (f.show_signal)
  {
    const float r = 3.0f * unit;
    const ImVec2 c(at.x + r, at.y + r);
    Kit::Connection(dl, c, r, unit,
                    f.signal == Signal::Good ? 3 :
                    f.signal == Signal::Ok   ? 2 :
                                               1);
    const int lines = (f.stats.empty() ? 0 : 1) + (f.perf.empty() ? 0 : 1);
    float ty = c.y - static_cast<float>(lines) * line_h / 2;
    const float tx = c.x + r + 0.9f * unit;
    if (!f.stats.empty())
      ty = Kit::Label(dl, ImVec2(tx, ty), unit, f.stats, SIZE).max.y;
    if (!f.perf.empty())
      ty = Kit::Label(dl, ImVec2(tx, ty), unit, f.perf, SIZE, Kit::Align::Left,
                      LevelColour(f.perf_level, 1))
               .max.y;
    y = std::max(c.y + r, ty) + 1.2f * unit;
  }
  else if (!f.perf.empty())
  {
    y = Kit::Label(dl, at, unit, f.perf, SIZE, Kit::Align::Left, LevelColour(f.perf_level, 1))
            .max.y +
        0.8f * unit;
  }
  if (!f.graph.empty())
  {
    const ImVec2 size(40 * unit, 9 * unit);
    FrameGraph(dl, ImVec2(at.x, y), size, unit, f.graph);
    y += size.y + 1.2f * unit;
  }
  return y;
}

// `note` in lines no wider than `width` in `look`, '\n' between them: broken at its " · " first,
// at spaces inside a part still too wide.
std::string FitNote(const Kit::TextLook& look, const std::string& note, float width)
{
  static const std::string sep = " \u00b7 ";
  std::vector<std::string> parts;
  for (std::size_t at = 0;;)
  {
    const std::size_t next = note.find(sep, at);
    parts.push_back(note.substr(at, next == std::string::npos ? std::string::npos : next - at));
    if (next == std::string::npos)
      break;
    at = next + sep.size();
  }
  std::vector<std::string> lines;
  for (const std::string& part : parts)
  {
    const std::string longer = lines.empty() ? part : lines.back() + sep + part;
    if (!lines.empty() && Kit::Measure(look, longer).x <= width)
      lines.back() = longer;
    else if (Kit::Measure(look, part).x <= width)
      lines.push_back(part);
    else
      for (std::string& l : Kit::Wrap(look, part, width))
        lines.push_back(std::move(l));
  }
  std::string out;
  for (const std::string& l : lines)
    out += (out.empty() ? "" : "\n") + l;
  return out;
}

// A banner's note fitted into `width` (its words' room: the banner's width less its cut ends and
// margin) for a banner whose first row is `row` tall: the note, and the banner's height.
std::pair<std::string, float> FitBanner(const std::string& title, const std::string& note,
                                        float row, float width, float unit)
{
  if (note.empty() || Kit::BannerWidth(row, unit, title, note) <= width)
    return {note, row};
  const std::string fitted =
      FitNote(Kit::InkLook(unit, row * 0.3f / unit), note, width - row * 0.6f - 4.0f * unit);
  const auto lines = static_cast<std::size_t>(std::count(fitted.begin(), fitted.end(), '\n') + 1);
  return {fitted, Kit::BannerHeight(row, lines)};
}

// Top centre: each pair of lines on a banner (the first, the search's or the queue's, with the
// turn's timer beside it on its plate). `guide` gets this machine's hint (the control guide's).
// Returns the lowest bottom and leftmost edge drawn (the toasts and the chat keep clear of them).
Kit::Box DrawTopCentre(ImDrawList* dl, const Scene& s, ImVec2 pic, ImVec2 pic_size, float unit,
                       double now_s, float first_top, std::string* guide)
{
  *guide = s.hint;
  Kit::Box drawn{ImVec2(pic.x + pic_size.x, pic.y), ImVec2(pic.x, pic.y)};
  const auto add = [&](const Kit::Box& b) {
    drawn.min.x = std::min(drawn.min.x, b.min.x);
    drawn.max.y = std::max(drawn.max.y, b.max.y);
  };
  std::vector<std::pair<std::string, std::string>> groups;
  for (const auto& [title, note] : {std::pair{s.search, s.search_note},
                                    std::pair{s.rules, s.rules_note}})
  {
    if (!title.empty())
      groups.emplace_back(title, note);
    else if (!note.empty())
      groups.emplace_back(note, std::string());
  }
  int seconds = -1;
  for (auto& [title, note] : groups)
  {
    if (seconds < 0)
      seconds = TakeTimer(&title);
    if (seconds < 0)
      seconds = TakeTimer(&note);
  }
  if (Kit::CurrentLook() == Kit::Look::ProjectPlus && Queue::OnQueueCss() && !groups.empty())
  {
    // Project+'s queue character select: the lines on its own rules bar (the band right of RANKED
    // that says "-stock Smash!", locked in a queue room anyway), as Brawl's rules bar says them in
    // the game's own text (NativeText.h): what happens now, the set under it; the turn's timer
    // under the bar's right end, over the grid's empty corner (the band's own when it has it), and
    // this machine's hint in the guide. The bar's place in the picture, measured on 1280x720
    // screenshots (2026-10-04): x 520..1265, y 42..105.
    std::string title = groups[0].first, note, hint = s.hint;
    if (!s.search.empty())
    {
      note = s.rules;
      const std::string& more = s.search_note.empty() ? s.rules_note : s.search_note;
      hint = hint.empty() ? more : more.empty() ? hint : hint + " \u00b7 " + more;
    }
    else
    {
      note = s.rules_note;
    }
    TakeTimer(&note);
    TakeTimer(&hint);
    const float left = pic.x + pic_size.x * 0.4f, right = pic.x + pic_size.x * 0.99f;
    const float top = pic.y + pic_size.y * 0.056f;
    const auto [fitted, h] = FitBanner(title, note, pic_size.y * 0.092f, right - left, unit);
    add(Kit::Banner(dl, left, right, top, h, unit, Kit::Tone::Panel, title, fitted));
    const float x = right - 1.5f * unit;
    const float y = top + h + 1.0f * unit;
    if (seconds >= 0)
      add(Kit::Timer(dl, ImVec2(x - 5.5f * unit, y + 2.4f * unit), unit, seconds, now_s));
    *guide = hint;
    return drawn;
  }
  const float cx = pic.x + pic_size.x / 2;
  float top = first_top;
  for (std::size_t i = 0; i < groups.size(); ++i)
  {
    const auto& [title, note] = groups[i];
    // The first banner the larger: what happens now; the second the set's. At most 62% of the
    // picture wide (the timer goes beside it): a longer note takes more lines.
    const float row =
        i == 0 ? (note.empty() ? 6.4f : 8.6f) * unit : (note.empty() ? 5.0f : 7.0f) * unit;
    const float max_w = 0.62f * pic_size.x;
    const auto [fitted, h] = FitBanner(title, note, row, max_w, unit);
    const float w = std::clamp(Kit::BannerWidth(h, unit, title, fitted), 26.0f * unit,
                               std::max(max_w, 26.0f * unit));
    const Kit::Box b = Kit::Banner(dl, cx - w / 2, cx + w / 2, top, h, unit, Kit::Tone::Panel,
                                   title, fitted);
    add(b);
    if (i == 0 && seconds >= 0)
    {
      add(Kit::Timer(dl, ImVec2(b.max.x + 6.0f * unit, (b.min.y + b.max.y) / 2), unit, seconds,
                     now_s));
    }
    top = b.max.y + 1.0f * unit;
  }
  return drawn;
}

void Draw()
{
  Kit::BeginFrame();
  const double now = NowMs();
  const Scene s = Gather(now);
  // The kit's demo in place of everything else (design checks).
  if (Kit::DemoMode() == 1)
  {
    DrawKitDemo();
    return;
  }
  // The chat cards' font, loaded before any chat needs it (YgCard.h).
  Yg::Warm();
  // The YouGame button and its lines (YgOrb.h, embed cap `yougame`), drawn with the rest.
  const YgOrb::Model::View yg = YgOrb::Current().At(now);
  if (s.Empty() && !yg.on && !yg.showing)
    return;
  // Everything is laid out in the game's picture (the window can letterbox or pillarbox it), in
  // its units (1% of its height), drawn with the kit (Kit.h): the game's fonts, its plates.
  ImVec2 pic, pic_size;
  PictureRect(&pic, &pic_size);
  const float unit = OverlayUnit(pic_size.y);
  // No picture to draw on (no backbuffer yet, a window the app squeezed to a sliver): nothing.
  if (unit <= 0 || !(pic_size.x >= 1))
    return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const double now_s = ImGui::GetTime();
  const OverlayModel::Frame& f = s.f;
  // The stage select's two cursors first: every line of text goes over them.
  if (s.cursors.on)
    DrawStageCursors(dl, s.cursors, pic, pic_size, unit, now_s);
  // The character select's panels: a name tab in the port's colour on each panel the game's own
  // plate doesn't name. Design F's portraits: a locked-in player's framed in gold with LOCKED IN
  // across its top, the picker's in their port's colour; the empty seat's ring while the queue
  // searches.
  for (int port = 0; port < 4; ++port)
  {
    const float centre = pic.x + pic_size.x * PanelCentre(port);
    if (!s.labels[port].empty())
    {
      Kit::LabelPlate(dl, ImVec2(centre, pic.y + pic_size.y * NAME_TAB_Y), unit, s.labels[port],
                      2.8f, Kit::Align::Centre, Kit::PortTone(port));
    }
    const Kit::Box portrait = PortraitRect(port, pic, pic_size);
    if (s.locked[port])
    {
      Kit::PortraitFrame(dl, portrait.min, portrait.max, unit, Kit::Tone::Gold);
      Kit::Ribbon(dl, ImVec2(portrait.min.x + 1.9f * unit, portrait.min.y + 1.4f * unit),
                  ImVec2(portrait.max.x - 1.9f * unit, portrait.min.y + 5.8f * unit), unit,
                  "LOCKED IN", Kit::Tone::Gold);
    }
    else if (port == s.picker)
    {
      Kit::PortraitFrame(dl, portrait.min, portrait.max, unit, Kit::PortTone(port));
    }
  }
  if (s.searching)
  {
    const Kit::Box seat = PortraitRect(1, pic, pic_size);
    Kit::SearchRing(dl, ImVec2((seat.min.x + seat.max.x) / 2, (seat.min.y + seat.max.y) / 2),
                    pic_size.y * 0.072f, unit, now_s);
  }
  if (s.dots.on)
  {
    Kit::SetDots(dl, ImVec2(pic.x + pic_size.x / 2, pic.y + DOTS_Y * unit), unit, s.dots.games,
                 s.dots.current);
  }
  // Top centre, then top left, where both games' matches and menus leave the screen empty.
  std::string guide;
  const Kit::Box centre = DrawTopCentre(dl, s, pic, pic_size, unit, now_s,
                                        pic.y + (s.dots.on ? TOP_UNDER_DOTS : TOP_FIRST) * unit,
                                        &guide);
  // Design F: the turn's timer in the lettering of the game's words that say whose turn it is,
  // and this machine's buttons in the games' own corner for them.
  const bool pplus = Kit::CurrentLook() == Kit::Look::ProjectPlus;
  if (s.band_seconds >= 0)
  {
    const GameTimerPlace& at = pplus ? BAND_TIMER_PPLUS : BAND_TIMER_BRAWL;
    Kit::GameTimer(dl, ImVec2(pic.x + pic_size.x * at.x, pic.y + pic_size.y * at.y), unit,
                   s.band_seconds, now_s, Kit::Lettering::Band, at.size);
  }
  if (s.line_seconds >= 0)
  {
    const GameTimerPlace& at = pplus ? LINE_TIMER_PPLUS : LINE_TIMER_BRAWL;
    const float half = Kit::Measure(Kit::TitleLook(unit, at.size), "0:00").x / 2;
    Kit::GameTimer(dl, ImVec2(pic.x + pic_size.x * at.x + half, pic.y + pic_size.y * at.y), unit,
                   s.line_seconds, now_s, Kit::Lettering::Line, at.size);
  }
  if (!guide.empty())
  {
    // One line along the bottom edge, the buttons and the words without one (a proposal, the
    // casual pick's rule) in the hint's order, drawn smaller when longer than the picture is wide
    // (never over the games' own buttons). Project+'s relabelled legend already says X, A and B on
    // its stage select. Parsed once per hint (it changes a few times a screen; the video thread).
    static std::string s_hint;
    static std::vector<Kit::GuideItem> s_items;
    if (guide != s_hint)
    {
      s_hint = guide;
      s_items = Kit::GuideOf(guide);
    }
    std::vector<Kit::GuideItem> items;
    for (const Kit::GuideItem& item : s_items)
    {
      if (!(s.legend && (item.button == "X" || item.button == "A" || item.button == "B")))
        items.push_back(item);
    }
    Kit::Guide(dl, pic.x + pic_size.x * GUIDE_RIGHT, pic.y + pic_size.y * GUIDE_Y, unit, items, 1,
               pic_size.x * (GUIDE_RIGHT - 0.025f));
  }
  const float margin = 2.5f * unit;
  // The corner and the column under it start right of the YouGame button when it is there.
  const Kit::Box orb = YgOrb::ButtonBox(yg);
  const float left =
      orb.max.x > orb.min.x ? std::max(pic.x + margin, orb.max.x + 1.2f * unit) : pic.x + margin;
  float y = DrawCorner(dl, f, ImVec2(left, pic.y + margin), unit);
  if (!f.waiting.empty())
  {
    // "Waiting for <name>…" under the top centre's banners.
    const float top = std::max(pic.y + pic_size.y * 0.18f, centre.max.y + 1.5f * unit);
    Kit::Title(dl, ImVec2(pic.x + pic_size.x / 2, top), unit, f.waiting, 5.0f, Kit::Align::Centre);
  }
  // The toasts and the chat on plates under the corner: under Dolphin's own messages (OSD rows
  // such as "Screenshot saved...", drawn at the top left just before this), and under the top
  // centre's banners where a plate as wide as the chat's would reach them.
  if (!f.toasts.empty() || !f.chats.empty() || yg.showing)
  {
    y = std::max(y, OSD::MessagesBottom() + unit);
    const float wrap = 0.36f * pic_size.x;
    if (centre.max.y > pic.y && centre.min.x < left + wrap + 3.0f * unit)
      y = std::max(y, centre.max.y + 1.2f * unit);
    const float bottom = pic.y + pic_size.y - margin;
    // The YouGame button's lines first, the nearest to it.
    y = YgOrb::DrawLines(dl, yg, ImVec2(left, y), unit, wrap, bottom, now);
    // In YouGame's own look (YgCard.h): the room's and the chat are YouGame's, not the game's.
    for (const OverlayModel::Toast& t : f.toasts)
    {
      if (y >= bottom)
        break;
      y = Yg::NoticeCard(dl, ImVec2(left, y), unit, t.text, wrap, t.alpha).max.y +
          0.8f * unit;
    }
    for (const OverlayModel::ChatToast& c : f.chats)
    {
      if (y >= bottom)
        break;
      y = Yg::ChatCard(dl, ImVec2(left, y), unit, c.name, c.text, wrap, c.alpha).max.y +
          0.8f * unit;
    }
  }
  // Over everything else: the set's end, the opponent's disconnect, set over's hint (SetEnd.h).
  if (!s.set_end.Empty())
    SetEnd::Draw(dl, unit, pic.x, pic.y, pic_size.x, pic_size.y, s.set_end);
  // The YouGame button over everything: it stands where the page's own is.
  YgOrb::DrawButton(dl, yg);
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
