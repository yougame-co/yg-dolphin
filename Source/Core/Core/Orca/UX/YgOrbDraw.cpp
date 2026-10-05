// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/YgOrbDraw.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Orca/UX/YgCard.h"

namespace Orca::UX::YgOrb
{
namespace
{
ImU32 Rgba(int r, int g, int b, float a)
{
  return IM_COL32(r, g, b, static_cast<int>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255)));
}

// The page's button at rest is half there, lit at full (hovered, peeking, blinking, a call or an
// invite waiting, a held seat), and at 0.85 with unread lines (globals.css .play-orb.top).
float ButtonAlpha(const Place& p)
{
  return p.lit ? 1.0f : p.badge > 0 ? 0.85f : 0.5f;
}

std::string BadgeText(int badge)
{
  return badge > 9 ? "9+" : std::to_string(badge);
}

// The badge's box (the page's .play-orb.top .play-menu-badge: 19 px high, at least 19 wide, 6 px
// over the top and 7 past the right), in the button's 40-pixel grid scaled by `k`.
Kit::Box BadgeBox(const Place& p, float k)
{
  const float px = 11.5f * k;
  const float w = std::max(19.0f * k, Yg::TextWidth(BadgeText(p.badge), px) + 8.0f * k);
  const float right = p.x + p.size + 7.0f * k, top = p.y - 6.0f * k;
  return Kit::Box{ImVec2(right - w, top), ImVec2(right, top + 19.0f * k)};
}
}  // namespace

Kit::Box ButtonBox(const Model::View& view)
{
  if (!view.on || view.place.size <= 0)
    return Kit::Box{ImVec2(0, 0), ImVec2(0, 0)};
  const Place p = ScaledPlace(view.place, view.view_width, ImGui::GetIO().DisplaySize.x);
  const float k = p.size / 40;
  Kit::Box box{ImVec2(p.x, p.y), ImVec2(p.x + p.size, p.y + p.size)};
  if (p.away)
  {
    box.max.x = std::max(box.max.x, p.x + p.size + 5.0f * k);
    box.max.y = std::max(box.max.y, p.y + p.size + 5.0f * k);
  }
  if (p.badge > 0)
  {
    const Kit::Box b = BadgeBox(p, k);
    box.min.y = std::min(box.min.y, b.min.y - 2.0f * k);
    box.max.x = std::max(box.max.x, b.max.x + 2.0f * k);
  }
  return box;
}

void DrawButton(ImDrawList* dl, const Model::View& view)
{
  if (!view.on || view.place.size <= 0)
    return;
  const Place p = ScaledPlace(view.place, view.view_width, ImGui::GetIO().DisplaySize.x);
  const float k = p.size / 40, alpha = ButtonAlpha(p);
  const ImVec2 c(p.x + p.size / 2, p.y + p.size / 2);
  Yg::RoundLogo(dl, ImVec2(p.x, p.y), p.size, alpha);
  // A blink: a white ring, 2 px, grows from the disc's edge to 2.1 times its size and fades.
  if (view.ring >= 0)
  {
    const float scale = 1 + 1.1f * view.ring;
    dl->AddCircle(c, (p.size / 2 - k) * scale, Rgba(255, 255, 255, 0.95f * (1 - view.ring)), 48,
                  2 * k * scale);
  }
  // A friend's held seat: an amber dot in a black ring at the bottom right.
  if (p.away)
  {
    const ImVec2 dot(p.x + p.size - 3.5f * k, p.y + p.size - 3.5f * k);
    dl->AddCircleFilled(dot, 8.5f * k, Rgba(0, 0, 0, alpha), 24);
    dl->AddCircleFilled(dot, 6.5f * k, Rgba(242, 161, 20, alpha), 24);
  }
  // Unread: a white badge with the count in red, in a black ring, at the top right.
  if (p.badge > 0)
  {
    const Kit::Box b = BadgeBox(p, k);
    const float r = (b.max.y - b.min.y) / 2;
    dl->AddRectFilled(ImVec2(b.min.x - 2 * k, b.min.y - 2 * k),
                      ImVec2(b.max.x + 2 * k, b.max.y + 2 * k), Rgba(0, 0, 0, alpha), r + 2 * k);
    dl->AddRectFilled(b.min, b.max, Rgba(255, 255, 255, alpha), r);
    const std::string text = BadgeText(p.badge);
    const float px = 11.5f * k;
    const float w = Yg::TextWidth(text, px);
    Yg::Text(dl, ImVec2((b.min.x + b.max.x - w) / 2, b.min.y + (b.max.y - b.min.y - px) / 2), px,
             Rgba(221, 0, 0, alpha), text);
  }
}

namespace
{
// A line laid out for a unit and a width: its words wrapped, its card's size, its hint's widths.
struct Laid
{
  Model::Entry entry;
  std::string said;  // "<name> <text>"
  std::vector<std::string> lines;
  float text_w = 0;
  float key_w = 0;   // the keycap, 0 without a hint
  float hint_w = 0;  // the gap, the keycap and the verb
  float h = 0;
};

struct Metrics
{
  float pad, px, r, gap, kpx, line_h, text_x;
};

Metrics MetricsFor(float unit)
{
  Metrics m;
  m.pad = 1.2f * unit;
  m.px = 1.95f * unit;
  m.r = 1.45f * unit;
  m.gap = 0.9f * unit;
  m.kpx = 1.6f * unit;
  m.line_h = m.px * 1.28f;
  m.text_x = m.pad + 2 * m.r + m.gap;
  return m;
}

Laid Layout(const Model::Entry& e, const Metrics& m, float unit, float width)
{
  Laid l;
  l.entry = e;
  const Notice& n = e.notice;
  if (!n.key.empty())
  {
    l.key_w = Yg::TextWidth(n.key, m.kpx) + 1.4f * unit;
    l.hint_w = m.gap + l.key_w + 0.6f * unit + Yg::TextWidth(n.verb, m.kpx);
  }
  l.said = n.name + " " + n.text;
  l.lines = Yg::Wrap(l.said, m.px, std::max(10.0f * unit, width - m.text_x - m.pad - l.hint_w));
  for (const std::string& s : l.lines)
    l.text_w = std::max(l.text_w, Yg::TextWidth(s, m.px));
  l.h = std::max(2 * m.r, m.line_h * static_cast<float>(std::max<std::size_t>(l.lines.size(), 1))) +
        2 * m.pad;
  return l;
}

// The lines as last laid out (the video thread only).
struct LineCache
{
  std::uint64_t generation = 0;
  std::vector<Model::Entry> entries;
  float unit = 0;
  float width = 0;
  std::vector<Laid> laid;
};
LineCache s_cache;
}  // namespace

float DrawLines(ImDrawList* dl, const Model::View& view, ImVec2 pos, float unit, float width,
                float bottom, double now_ms)
{
  if (!view.showing)
    return pos.y;
  // One line each, in the Pill's words: the person's avatar with what happened as a mark on it,
  // their name, the line, and the action's key and verb in grey when there is one. Laid out once
  // per new line (or a new size), then only drawn.
  LineCache& c = s_cache;
  const Metrics m = MetricsFor(unit);
  if (view.generation != c.generation)
  {
    c.generation = Current().Lines(c.generation, &c.entries);
    c.unit = 0;  // laid out again below
  }
  if (unit != c.unit || width != c.width)
  {
    c.unit = unit;
    c.width = width;
    c.laid.clear();
    for (const Model::Entry& e : c.entries)
      c.laid.push_back(Layout(e, m, unit, width));
  }
  float y = pos.y;
  for (const Laid& l : c.laid)
  {
    if (y >= bottom)
      break;
    const float a = LineAlpha(l.entry.notice, l.entry.at, now_ms);
    if (a <= 0)
      continue;
    const Notice& n = l.entry.notice;
    const ImVec2 min(pos.x, y);
    const ImVec2 max(pos.x + std::min(width, m.text_x + l.text_w + l.hint_w + m.pad), y + l.h);
    Yg::CardSlab(dl, min, max, unit, a);
    // The avatar, and the mark of what happened (the page's: a voice or call ring, a join's plus,
    // a leaver greyed, a dropped player's amber dot, a returning one's green dot).
    const ImVec2 ctr(pos.x + m.pad + m.r, y + l.h / 2);
    if (n.kind == Kind::Voice || n.kind == Kind::Call)
      dl->AddCircle(ctr, m.r + 0.35f * unit, Rgba(74, 222, 128, a), 32, 0.3f * unit);
    Yg::Avatar(dl, ctr, m.r, n.name, n.kind == Kind::Leave ? 0.45f * a : a);
    const ImVec2 mark(ctr.x + m.r * 0.72f, ctr.y + m.r * 0.72f);
    if (n.kind == Kind::Join || n.kind == Kind::Back || n.kind == Kind::Away)
    {
      dl->AddCircleFilled(mark, m.r * 0.42f, Rgba(15, 23, 42, a), 16);
      dl->AddCircleFilled(mark, m.r * 0.3f,
                          n.kind == Kind::Away ? Rgba(242, 161, 20, a) : Rgba(74, 222, 128, a),
                          16);
      if (n.kind == Kind::Join)
      {
        const float s = m.r * 0.17f, t = std::max(1.0f, 0.12f * unit);
        dl->AddLine(ImVec2(mark.x - s, mark.y), ImVec2(mark.x + s, mark.y), Rgba(15, 23, 42, a), t);
        dl->AddLine(ImVec2(mark.x, mark.y - s), ImVec2(mark.x, mark.y + s), Rgba(15, 23, 42, a), t);
      }
    }
    // The words: the name bright, the rest of the line softer.
    const float tx = pos.x + m.text_x;
    float ty = y + (l.h - m.line_h * static_cast<float>(l.lines.size())) / 2 + 0.1f * unit;
    for (std::size_t i = 0; i < l.lines.size(); ++i)
    {
      const std::string& s = l.lines[i];
      if (i == 0 && s.starts_with(n.name))
      {
        Yg::Text(dl, ImVec2(tx, ty), m.px, Yg::Ink(a), n.name);
        Yg::Text(dl, ImVec2(tx + Yg::TextWidth(n.name, m.px), ty), m.px, Yg::Ink2(a),
                 std::string_view(s).substr(n.name.size()));
      }
      else
      {
        Yg::Text(dl, ImVec2(tx, ty), m.px, Yg::Ink2(a), s);
      }
      ty += m.line_h;
    }
    // The hint, grey, at the right of the last line: the key in a keycap, then the verb.
    if (l.key_w > 0)
    {
      const float hy = ty - m.line_h + (m.px - m.kpx) / 2;
      const float kx = max.x - m.pad - l.hint_w + m.gap;
      const ImVec2 kmin(kx, hy - 0.25f * unit), kmax(kx + l.key_w, hy + m.kpx + 0.25f * unit);
      dl->AddRect(kmin, kmax, Yg::Edge(0.7f * a), 0.5f * unit, 0, std::max(1.0f, 0.12f * unit));
      Yg::Text(dl, ImVec2(kx + 0.7f * unit, hy), m.kpx, Yg::Ink2(0.85f * a), n.key);
      Yg::Text(dl, ImVec2(kmax.x + 0.6f * unit, hy), m.kpx, Yg::Ink2(0.85f * a), n.verb);
    }
    y = max.y + 0.8f * unit;
  }
  return y;
}
}  // namespace Orca::UX::YgOrb
