// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>

#include "Core/Orca/UX/Kit.h"

namespace Orca::UX::Widgets
{
namespace
{
ImU32 Rgba(int r, int g, int b, float a = 1)
{
  return IM_COL32(r, g, b, static_cast<int>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255)));
}

// A text size the kit takes: at least a pixel, never a NaN.
float Px(float size)
{
  return size >= 1.0f ? size : 1.0f;
}

// The tone's colours, top to bottom, for words on the panel. Neutral is the panel's own ink.
void Colours(Tone tone, Kit::TextLook* look)
{
  switch (tone)
  {
  case Tone::Win:
    look->top = Rgba(255, 246, 170);
    look->bottom = Rgba(255, 190, 24);
    break;
  case Tone::Loss:
    look->top = Rgba(255, 176, 168);
    look->bottom = Rgba(236, 44, 40);
    break;
  case Tone::Warning:
    look->top = Rgba(255, 236, 170);
    look->bottom = Rgba(255, 164, 36);
    break;
  default:
    break;
  }
}

// The body face at `size` pixels, in the panel's ink (or the tone's colours), outlined dark.
Kit::TextLook LineLook(float size, Tone tone)
{
  Kit::TextLook look = Kit::InkLook(1.0f, Px(size));
  if (tone != Tone::Neutral)
  {
    Colours(tone, &look);
    look.outline = std::max(1.0f, look.px * 0.08f);
    look.outline_colour = Rgba(14, 8, 8);
    look.shadow = ImVec2(0, std::max(1.0f, look.px * 0.06f));
    look.shadow_colour = Rgba(0, 0, 0, 0.45f);
  }
  return look;
}

// The heavy face, italic, outlined: VICTORY in gold, DEFEAT in red, the rest in the look's title
// colours.
Kit::TextLook TitleLook(float unit, float size_units, Tone tone)
{
  Kit::TextLook look = Kit::TitleLook(unit, size_units);
  Colours(tone, &look);
  return look;
}

// The rating's digits: the heavy face, white into the panel's pale.
Kit::TextLook NumberLook(float unit, float scale)
{
  Kit::TextLook look = Kit::TitleLook(unit, NUMBER_SIZE * scale);
  look.top = Rgba(255, 255, 255);
  look.bottom = Rgba(214, 222, 236);
  look.italic = 0.06f;
  return look;
}

std::string DeltaText(int delta)
{
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), delta < 0 ? "-%d" : "+%d", delta < 0 ? -delta : delta);
  return buffer;
}
constexpr float DELTA_SIZE = LINE_SIZE * 1.1f;
Kit::Tone DeltaTone(int delta)
{
  return delta > 0 ? Kit::Tone::Green : delta < 0 ? Kit::Tone::Red : Kit::Tone::Grey;
}
}  // namespace

ImVec2 Measure(float size, std::string_view text)
{
  return Kit::Measure(LineLook(size, Tone::Neutral), text);
}

ImVec2 MeasureTitle(float unit, std::string_view text)
{
  return Kit::Measure(TitleLook(unit, TITLE_SIZE, Tone::Win), text);
}

ImVec2 MeasureNumber(float unit, std::string_view digits)
{
  return Kit::Measure(NumberLook(unit, 1), digits);
}

void Panel(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone, float alpha)
{
  Kit::Plate(dl, min, max, unit, Kit::Tone::Panel, alpha);
  if (tone == Tone::Neutral || alpha <= 0)
    return;
  // A thin line in the tone's colour inside the rim, as the game rings a highlighted panel.
  const float inset = 0.85f * unit;
  const float round = std::clamp(std::min(max.y - min.y, max.x - min.x) * 0.3f, 0.5f * unit,
                                 2.2f * unit);
  const ImU32 colour = tone == Tone::Win  ? Rgba(255, 206, 60, 0.9f * alpha) :
                       tone == Tone::Loss ? Rgba(236, 60, 52, 0.9f * alpha) :
                                            Rgba(255, 172, 44, 0.9f * alpha);
  dl->AddRect(ImVec2(min.x + inset, min.y + inset), ImVec2(max.x - inset, max.y - inset), colour,
              std::max(0.0f, round - inset), ImDrawFlags_None, std::max(1.0f, 0.3f * unit));
}

ImVec2 Title(ImDrawList* dl, float centre_x, float top, float unit, std::string_view text,
             Tone tone, float alpha, float scale)
{
  const ImVec2 s = MeasureTitle(unit, text);
  const float k = scale > 0 ? scale : 1;
  const Kit::TextLook look = TitleLook(unit, TITLE_SIZE * k, tone);
  const ImVec2 sk = Kit::Measure(look, text);
  Kit::Text(dl, ImVec2(centre_x, top + (s.y - sk.y) / 2), look, text, alpha, Kit::Align::Centre);
  return s;
}

ImVec2 Line(ImDrawList* dl, float centre_x, float top, float size, std::string_view text, Tone tone,
            float alpha)
{
  return Kit::Text(dl, ImVec2(centre_x, top), LineLook(size, tone), text, alpha,
                   Kit::Align::Centre);
}

ImVec2 BigNumber(ImDrawList* dl, float centre_x, float top, float unit, int value, float alpha,
                 float scale)
{
  char buffer[16];
  const int n = std::snprintf(buffer, sizeof(buffer), "%d", value);
  const std::string_view text(buffer, n > 0 ? static_cast<size_t>(n) : 0);
  const ImVec2 s = MeasureNumber(unit, text);
  const Kit::TextLook look = NumberLook(unit, scale > 0 ? scale : 1);
  const ImVec2 sk = Kit::Measure(look, text);
  Kit::Text(dl, ImVec2(centre_x, top + (s.y - sk.y) / 2), look, text, alpha, Kit::Align::Centre);
  return s;
}

float CountdownRadius(float unit)
{
  return 5.2f * unit;
}

float Countdown(ImDrawList* dl, ImVec2 centre, float unit, int seconds, float fraction, Tone tone,
                float alpha)
{
  const float r = CountdownRadius(unit);
  const float pi = std::numbers::pi_v<float>;
  // The panel's disc, a dark track round its body, and what is left of the wait lit in the tone.
  Kit::Disc(dl, centre, r, unit, Kit::Tone::Panel, alpha);
  const float track_r = r * 0.74f, thick = 0.9f * unit;
  dl->AddCircle(centre, track_r, Rgba(0, 0, 0, 0.45f * alpha), 48,
                thick + std::max(1.0f, 0.3f * unit));
  const float f = std::clamp(fraction, 0.0f, 1.0f);
  if (f > 0)
  {
    // From twelve o'clock, clockwise.
    const float a0 = -pi / 2, a1 = a0 + 2 * pi * f;
    const ImU32 lit = tone == Tone::Loss ? Rgba(255, 70, 60, alpha) :
                      tone == Tone::Win  ? Rgba(255, 210, 60, alpha) :
                                           Rgba(255, 180, 50, alpha);
    dl->PathArcTo(centre, track_r, a0, a1, std::max(8, static_cast<int>(48 * f)));
    dl->PathStroke(lit, ImDrawFlags_None, thick);
  }
  // The seconds alone inside the ring (the line beside it says "0:09"), the timer's digits.
  char buffer[16];
  const int n = std::snprintf(buffer, sizeof(buffer), "%d", std::clamp(seconds, 0, 999));
  const std::string_view text(buffer, n > 0 ? static_cast<size_t>(n) : 0);
  Kit::TextLook look = Kit::TitleLook(unit, COUNTDOWN_SIZE * (text.size() > 2 ? 0.6f : 0.85f));
  look.italic = 0.06f;
  look.top = Rgba(255, 255, 255);
  look.bottom = Rgba(214, 222, 236);
  if (seconds <= 5)
  {
    look.top = Rgba(255, 176, 168);
    look.bottom = Rgba(255, 44, 40);
  }
  Kit::Text(dl, ImVec2(centre.x, centre.y - look.px / 2 + look.px * 0.04f), look, text, alpha,
            Kit::Align::Centre);
  return r;
}

ImVec2 DeltaBadgeSize(float unit, int delta)
{
  return Kit::BadgeSize(unit, DeltaText(delta), DeltaTone(delta), DELTA_SIZE);
}

ImVec2 DeltaBadge(ImDrawList* dl, float left, float centre_y, float unit, int delta, float alpha,
                  float scale)
{
  const ImVec2 size = DeltaBadgeSize(unit, delta);
  const float k = scale > 0 ? scale : 1;
  const Kit::Tone tone = DeltaTone(delta);
  // Scaled about its centre: the badge drawn at `k` times its size and unit.
  Kit::Badge(dl, ImVec2(left + size.x / 2, centre_y), unit * k, DeltaText(delta), tone, DELTA_SIZE,
             alpha);
  return size;
}

ImVec2 Hint(ImDrawList* dl, float centre_x, float top, float unit, std::string_view text,
            float progress, float alpha)
{
  // A little larger than the other small lines: set over's only way out.
  const Kit::TextLook look = Kit::InkLook(unit, HINT_SIZE);
  const ImVec2 ts = Kit::Measure(look, text);
  const float pad_x = 2.0f * unit, pad_y = 0.7f * unit, bar = 0.55f * unit;
  const ImVec2 box(ts.x + 2 * pad_x, ts.y + 2 * pad_y + bar + 0.4f * unit);
  const ImVec2 min(centre_x - box.x / 2, top), max(centre_x + box.x / 2, top + box.y);
  Kit::Plate(dl, min, max, unit, Kit::Tone::Panel, alpha);
  Kit::Text(dl, ImVec2(centre_x, top + pad_y), look, text, alpha, Kit::Align::Centre);
  // The hold's bar under the text: a dark track, filling in gold left to right.
  const float y = top + pad_y + ts.y + 0.2f * unit;
  const float x0 = centre_x - ts.x / 2 + look.outline, x1 = centre_x + ts.x / 2 - look.outline;
  dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + bar), Rgba(0, 0, 0, 0.42f * alpha), bar / 2);
  const float p = std::clamp(progress, 0.0f, 1.0f);
  if (p > 0)
  {
    const ImU32 top_c = Rgba(255, 240, 150, alpha), bottom_c = Rgba(244, 164, 0, alpha);
    dl->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * p, y + bar), top_c, top_c,
                                bottom_c, bottom_c);
  }
  return box;
}

std::string_view ClockText(int seconds, char (&buffer)[16])
{
  seconds = std::clamp(seconds, 0, 99 * 60 + 59);
  const int n = std::snprintf(buffer, sizeof(buffer), "%d:%02d", seconds / 60, seconds % 60);
  return std::string_view(buffer, n > 0 ? static_cast<size_t>(n) : 0);
}
}  // namespace Orca::UX::Widgets
