// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/YgOrb.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <system_error>

#include "Core/Orca/UX/Chat.h"
#include "Core/Orca/UX/YgCard.h"

namespace Orca::UX::YgOrb
{
namespace
{
// The line split at single spaces (no empty fields: "a  b" is malformed).
std::vector<std::string_view> Fields(std::string_view line)
{
  std::vector<std::string_view> out;
  std::size_t at = 0;
  while (at <= line.size())
  {
    const std::size_t space = line.find(' ', at);
    const std::size_t end = space == std::string_view::npos ? line.size() : space;
    out.push_back(line.substr(at, end - at));
    if (space == std::string_view::npos)
      break;
    at = space + 1;
  }
  return out;
}

// A whole decimal number in [lo, hi], digits only.
std::optional<int> Number(std::string_view field, int lo, int hi)
{
  if (field.empty() || field.size() > 5 ||
      !std::all_of(field.begin(), field.end(), [](char c) { return c >= '0' && c <= '9'; }))
    return std::nullopt;
  int value = 0;
  const auto [end, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
  if (ec != std::errc() || end != field.data() + field.size() || value < lo || value > hi)
    return std::nullopt;
  return value;
}

std::optional<Kind> KindOf(std::string_view field)
{
  if (field == "join")
    return Kind::Join;
  if (field == "leave")
    return Kind::Leave;
  if (field == "away")
    return Kind::Away;
  if (field == "back")
    return Kind::Back;
  if (field == "voice")
    return Kind::Voice;
  if (field == "call")
    return Kind::Call;
  if (field == "dm")
    return Kind::Dm;
  if (field == "invite")
    return Kind::Invite;
  return std::nullopt;
}

std::size_t CodePoints(std::string_view text)
{
  return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) {
    return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  }));
}

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

std::optional<Command> ParseOrb(std::string_view line)
{
  const std::vector<std::string_view> f = Fields(line);
  if (f.empty() || f[0] != "orb")
    return std::nullopt;
  Command c;
  if (f.size() == 2 && f[1] == "off")
  {
    c.type = Command::Type::Off;
    return c;
  }
  if (f.size() == 2 && f[1] == "blink")
  {
    c.type = Command::Type::Blink;
    return c;
  }
  if (f.size() != 7)
    return std::nullopt;
  const std::optional<int> x = Number(f[1], 0, 32767), y = Number(f[2], 0, 32767),
                           size = Number(f[3], 8, 1024), lit = Number(f[4], 0, 1),
                           away = Number(f[5], 0, 1), badge = Number(f[6], 0, 99);
  if (!x || !y || !size || !lit || !away || !badge)
    return std::nullopt;
  c.type = Command::Type::Place;
  c.place = Place{static_cast<float>(*x),
                  static_cast<float>(*y),
                  static_cast<float>(*size),
                  *lit == 1,
                  *away == 1,
                  *badge};
  return c;
}

std::optional<Notice> ParseNotice(std::string_view line)
{
  const std::vector<std::string_view> f = Fields(line);
  if ((f.size() != 4 && f.size() != 6) || f[0] != "notice")
    return std::nullopt;
  const std::optional<Kind> kind = KindOf(f[1]);
  if (!kind)
    return std::nullopt;
  Notice n;
  n.kind = *kind;
  std::optional<std::string> name = Chat::DecodeField(f[2], Chat::NAME_ENCODED, Chat::NAME_POINTS);
  std::optional<std::string> text = Chat::DecodeField(f[3], Chat::TEXT_ENCODED, Chat::TEXT_POINTS);
  if (!name || !text)
    return std::nullopt;
  n.name = std::move(*name);
  n.text = std::move(*text);
  if (f.size() == 6)
  {
    std::optional<std::string> key = Chat::DecodeField(f[4], HINT_POINTS * 12, HINT_POINTS);
    std::optional<std::string> verb = Chat::DecodeField(f[5], HINT_POINTS * 12, HINT_POINTS);
    if (!key || !verb)
      return std::nullopt;
    n.key = std::move(*key);
    n.verb = std::move(*verb);
  }
  return n;
}

double DwellMs(const Notice& notice)
{
  switch (notice.kind)
  {
  case Kind::Voice:
  case Kind::Call:
    return 8000;
  case Kind::Join:
    return 3500;
  case Kind::Leave:
  case Kind::Back:
    return 2500;
  case Kind::Away:
    return 5000;
  case Kind::Dm:
  case Kind::Invite:
    break;
  }
  // A message: long enough to read (4 s, and 60 ms a character, up to 9 s).
  return std::min(9000.0, 4000.0 + 60.0 * static_cast<double>(CodePoints(notice.text)));
}

void Model::Apply(const Command& command, double now_ms)
{
  std::lock_guard lock(m_lock);
  switch (command.type)
  {
  case Command::Type::Off:
    m_on = false;
    m_blink_at = -1;
    break;
  case Command::Type::Blink:
    m_blink_at = now_ms;
    break;
  case Command::Type::Place:
    m_on = true;
    m_place = command.place;
    break;
  }
}

void Model::Add(Notice notice, double now_ms)
{
  std::lock_guard lock(m_lock);
  std::erase_if(m_lines, [&](const std::pair<double, Notice>& l) {
    return now_ms - l.first >= DwellMs(l.second);
  });
  m_lines.emplace_back(now_ms, std::move(notice));
  while (m_lines.size() > KEEP)
    m_lines.pop_front();
}

Model::View Model::At(double now_ms) const
{
  std::lock_guard lock(m_lock);
  View v;
  v.on = m_on;
  v.place = m_place;
  if (m_on && m_blink_at >= 0)
  {
    const double age = now_ms - m_blink_at;
    if (age >= 0 && age < RING_MS * RINGS)
      v.ring = static_cast<float>(std::fmod(age, RING_MS) / RING_MS);
  }
  for (const auto& [at, notice] : m_lines)
  {
    const double left = DwellMs(notice) - (now_ms - at);
    if (left <= 0)
      continue;
    v.lines.push_back(Line{notice, static_cast<float>(std::min(1.0, left / FADE_MS))});
  }
  return v;
}

bool Model::Hit(float x, float y) const
{
  std::lock_guard lock(m_lock);
  if (!m_on || m_place.size <= 0)
    return false;
  const float r = m_place.size / 2;
  const float dx = x - (m_place.x + r), dy = y - (m_place.y + r);
  return dx * dx + dy * dy <= r * r;
}

Model& Current()
{
  static Model s_model;
  return s_model;
}

double NowMs()
{
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void Apply(const Command& command)
{
  Current().Apply(command, NowMs());
}

void Show(Notice notice)
{
  Current().Add(std::move(notice), NowMs());
}

Kit::Box ButtonBox(const Model::View& view)
{
  const Place& p = view.place;
  if (!view.on || p.size <= 0)
    return Kit::Box{ImVec2(0, 0), ImVec2(0, 0)};
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
  const Place& p = view.place;
  if (!view.on || p.size <= 0)
    return;
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

float DrawLines(ImDrawList* dl, const Model::View& view, ImVec2 pos, float unit, float width,
                float bottom)
{
  // One line each, in the Pill's words: the person's avatar with what happened as a mark on it,
  // their name, the line, and the action's key and verb in grey when there is one.
  const float pad = 1.2f * unit, px = 1.95f * unit, r = 1.45f * unit, gap = 0.9f * unit;
  const float kpx = 1.6f * unit, line_h = px * 1.28f;
  float y = pos.y;
  for (const Model::Line& l : view.lines)
  {
    if (y >= bottom)
      break;
    const Notice& n = l.notice;
    const float a = l.alpha;
    const bool hint = !n.key.empty();
    const float key_w = hint ? Yg::TextWidth(n.key, kpx) + 1.4f * unit : 0;
    const float hint_w = hint ? gap + key_w + 0.6f * unit + Yg::TextWidth(n.verb, kpx) : 0;
    const float text_x = pad + 2 * r + gap;
    const float room = std::max(10.0f * unit, width - text_x - pad - hint_w);
    const std::string said = n.name + " " + n.text;
    const std::vector<std::string> lines = Yg::Wrap(said, px, room);
    float text_w = 0;
    for (const std::string& s : lines)
      text_w = std::max(text_w, Yg::TextWidth(s, px));
    const float h =
        std::max(2 * r, line_h * static_cast<float>(std::max<std::size_t>(lines.size(), 1))) +
        2 * pad;
    const ImVec2 min(pos.x, y);
    const ImVec2 max(pos.x + std::min(width, text_x + text_w + hint_w + pad), y + h);
    Yg::CardSlab(dl, min, max, unit, a);
    // The avatar, and the mark of what happened (the page's: a voice or call ring, a join's plus,
    // a leaver greyed, a dropped player's amber dot, a returning one's green dot).
    const ImVec2 c(pos.x + pad + r, y + h / 2);
    if (n.kind == Kind::Voice || n.kind == Kind::Call)
      dl->AddCircle(c, r + 0.35f * unit, Rgba(74, 222, 128, a), 32, 0.3f * unit);
    Yg::Avatar(dl, c, r, n.name, n.kind == Kind::Leave ? 0.45f * a : a);
    const ImVec2 mark(c.x + r * 0.72f, c.y + r * 0.72f);
    if (n.kind == Kind::Join || n.kind == Kind::Back || n.kind == Kind::Away)
    {
      dl->AddCircleFilled(mark, r * 0.42f, Rgba(15, 23, 42, a), 16);
      dl->AddCircleFilled(mark, r * 0.3f,
                          n.kind == Kind::Away ? Rgba(242, 161, 20, a) : Rgba(74, 222, 128, a),
                          16);
      if (n.kind == Kind::Join)
      {
        const float s = r * 0.17f, t = std::max(1.0f, 0.12f * unit);
        dl->AddLine(ImVec2(mark.x - s, mark.y), ImVec2(mark.x + s, mark.y), Rgba(15, 23, 42, a), t);
        dl->AddLine(ImVec2(mark.x, mark.y - s), ImVec2(mark.x, mark.y + s), Rgba(15, 23, 42, a), t);
      }
    }
    // The words: the name bright, the rest of the line softer.
    const float tx = pos.x + text_x;
    float ty = y + (h - line_h * static_cast<float>(lines.size())) / 2 + 0.1f * unit;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
      const std::string& s = lines[i];
      if (i == 0 && s.starts_with(n.name))
      {
        Yg::Text(dl, ImVec2(tx, ty), px, Yg::Ink(a), n.name);
        Yg::Text(dl, ImVec2(tx + Yg::TextWidth(n.name, px), ty), px, Yg::Ink2(a),
                 std::string_view(s).substr(n.name.size()));
      }
      else
      {
        Yg::Text(dl, ImVec2(tx, ty), px, Yg::Ink2(a), s);
      }
      ty += line_h;
    }
    // The hint, grey, at the right of the last line: the key in a keycap, then the verb.
    if (hint)
    {
      const float hy = ty - line_h + (px - kpx) / 2;
      const float kx = max.x - pad - hint_w + gap;
      const ImVec2 kmin(kx, hy - 0.25f * unit), kmax(kx + key_w, hy + kpx + 0.25f * unit);
      dl->AddRect(kmin, kmax, Yg::Edge(0.7f * a), 0.5f * unit, 0, std::max(1.0f, 0.12f * unit));
      Yg::Text(dl, ImVec2(kx + 0.7f * unit, hy), kpx, Yg::Ink2(0.85f * a), n.key);
      Yg::Text(dl, ImVec2(kmax.x + 0.6f * unit, hy), kpx, Yg::Ink2(0.85f * a), n.verb);
    }
    y = max.y + 0.8f * unit;
  }
  return y;
}
}  // namespace Orca::UX::YgOrb
