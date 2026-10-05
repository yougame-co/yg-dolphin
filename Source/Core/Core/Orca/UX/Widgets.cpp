// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Widgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>

namespace Orca::UX::Widgets
{
namespace
{
struct Rgb
{
  int r, g, b;
};

// The tone's colour, and the lighter one its capitals' top half takes (a metal sheen).
Rgb ColourOf(Tone tone)
{
  switch (tone)
  {
  case Tone::Win:
    return {255, 204, 40};
  case Tone::Loss:
    return {236, 66, 56};
  case Tone::Warning:
    return {255, 172, 44};
  default:
    return {255, 255, 255};
  }
}
Rgb SheenOf(Tone tone)
{
  switch (tone)
  {
  case Tone::Win:
    return {255, 244, 178};
  case Tone::Loss:
    return {255, 158, 148};
  case Tone::Warning:
    return {255, 226, 160};
  default:
    return {255, 255, 255};
  }
}

ImU32 Rgba(Rgb c, float a)
{
  return IM_COL32(c.r, c.g, c.b, static_cast<int>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255)));
}
ImU32 Rgba(int r, int g, int b, float a)
{
  return Rgba(Rgb{r, g, b}, a);
}

// A text size ImGui takes: at least a pixel (it asserts on 0), never a NaN.
float Px(float size)
{
  return size >= 1.0f ? size : 1.0f;
}

// `text` at `pos`, `size` pixels: a dark outline `o` wide all round and a drop shadow, the fill
// `fill`, and (with `sheen`) the top part of each glyph lighter.
void Outlined(ImDrawList* dl, float size, ImVec2 pos, std::string_view text, ImU32 fill, float o,
              float alpha, const ImU32* sheen = nullptr)
{
  ImFont* font = ImGui::GetFont();
  size = Px(size);
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const ImU32 dark = Rgba(14, 14, 22, alpha * 0.9f);
  dl->AddText(font, size, ImVec2(pos.x + o * 0.6f, pos.y + o * 1.4f), Rgba(0, 0, 0, alpha * 0.5f),
              begin, end);
  for (int i = 0; i < 8; ++i)
  {
    const float a = std::numbers::pi_v<float> * 2 * static_cast<float>(i) / 8;
    dl->AddText(font, size, ImVec2(pos.x + o * std::cos(a), pos.y + o * std::sin(a)), dark, begin,
                end);
  }
  dl->AddText(font, size, pos, fill, begin, end);
  if (sheen)
  {
    // The upper 45% of the line in the lighter colour, clipped.
    const ImVec2 s = font->CalcTextSizeA(size, FLT_MAX, 0, begin, end);
    dl->PushClipRect(pos, ImVec2(pos.x + s.x + 1, pos.y + s.y * 0.45f), true);
    dl->AddText(font, size, pos, *sheen, begin, end);
    dl->PopClipRect();
  }
}
}  // namespace

ImVec2 Measure(float size, std::string_view text)
{
  return ImGui::GetFont()->CalcTextSizeA(Px(size), FLT_MAX, 0, text.data(),
                                         text.data() + text.size());
}

void Panel(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone, float alpha)
{
  const float round = 1.6f * unit;
  // A soft shadow, the plate (a little lighter at the top), the rim: dark, then the tone's colour.
  dl->AddRectFilled(ImVec2(min.x + 0.5f * unit, min.y + 0.8f * unit),
                    ImVec2(max.x + 0.5f * unit, max.y + 0.8f * unit), Rgba(0, 0, 0, alpha * 0.35f),
                    round);
  dl->AddRectFilled(min, max, Rgba(12, 16, 32, alpha * 0.84f), round);
  dl->AddRectFilled(ImVec2(min.x + 0.4f * unit, min.y + 0.4f * unit),
                    ImVec2(max.x - 0.4f * unit, min.y + (max.y - min.y) * 0.42f),
                    Rgba(255, 255, 255, alpha * 0.05f), round * 0.8f);
  dl->AddRect(min, max, Rgba(6, 6, 12, alpha * 0.9f), round, ImDrawFlags_None,
              std::max(1.0f, 0.7f * unit));
  dl->AddRect(ImVec2(min.x + 0.25f * unit, min.y + 0.25f * unit),
              ImVec2(max.x - 0.25f * unit, max.y - 0.25f * unit),
              Rgba(ColourOf(tone), alpha * (tone == Tone::Neutral ? 0.45f : 0.9f)), round * 0.9f,
              ImDrawFlags_None, std::max(1.0f, 0.32f * unit));
}

ImVec2 Title(ImDrawList* dl, float centre_x, float top, float unit, std::string_view text,
             Tone tone, float alpha, float scale)
{
  const float size = TITLE_SIZE * unit;
  const ImVec2 s = Measure(size, text);
  const float k = scale > 0 ? scale : 1;
  const ImVec2 sk = Measure(size * k, text);
  const ImVec2 pos(centre_x - sk.x / 2, top + (s.y - sk.y) / 2);
  const ImU32 sheen = Rgba(SheenOf(tone), alpha);
  Outlined(dl, size * k, pos, text, Rgba(ColourOf(tone), alpha), std::max(1.5f, size * k / 16),
           alpha, &sheen);
  return s;
}

ImVec2 Line(ImDrawList* dl, float centre_x, float top, float size, std::string_view text, Tone tone,
            float alpha)
{
  const ImVec2 s = Measure(size, text);
  Outlined(dl, size, ImVec2(centre_x - s.x / 2, top), text, Rgba(ColourOf(tone), alpha),
           std::max(1.0f, size / 14), alpha);
  return s;
}

ImVec2 BigNumber(ImDrawList* dl, float centre_x, float top, float unit, int value, float alpha,
                 float scale)
{
  char buffer[16];
  const int n = std::snprintf(buffer, sizeof(buffer), "%d", value);
  const std::string_view text(buffer, n > 0 ? static_cast<size_t>(n) : 0);
  const float size = NUMBER_SIZE * unit;
  const ImVec2 s = Measure(size, text);
  const float k = scale > 0 ? scale : 1;
  const ImVec2 sk = Measure(size * k, text);
  const ImU32 sheen = Rgba(SheenOf(Tone::Neutral), alpha);
  Outlined(dl, size * k, ImVec2(centre_x - sk.x / 2, top + (s.y - sk.y) / 2), text,
           Rgba(236, 240, 255, alpha), std::max(1.5f, size * k / 15), alpha, &sheen);
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
  const float thick = 0.9f * unit;
  const float pi = std::numbers::pi_v<float>;
  dl->AddCircleFilled(centre, r, Rgba(8, 10, 20, alpha * 0.85f), 48);
  dl->AddCircle(centre, r - thick, Rgba(255, 255, 255, alpha * 0.16f), 48, thick);
  const float f = std::clamp(fraction, 0.0f, 1.0f);
  if (f > 0)
  {
    // From twelve o'clock, clockwise, what is left of the wait.
    const float a0 = -pi / 2, a1 = a0 + 2 * pi * f;
    dl->PathArcTo(centre, r - thick, a0, a1, std::max(8, static_cast<int>(48 * f)));
    dl->PathStroke(Rgba(ColourOf(tone), alpha), ImDrawFlags_None, thick);
  }
  dl->AddCircle(centre, r, Rgba(6, 6, 12, alpha * 0.9f), 48, std::max(1.0f, 0.3f * unit));
  // The seconds alone inside the ring (the line beside it says "0:09").
  char buffer[16];
  const int n = std::snprintf(buffer, sizeof(buffer), "%d", std::clamp(seconds, 0, 999));
  const std::string_view text(buffer, n > 0 ? static_cast<size_t>(n) : 0);
  const float size = COUNTDOWN_SIZE * unit * (text.size() > 2 ? 0.7f : 1.0f);
  const ImVec2 s = Measure(size, text);
  Outlined(dl, size, ImVec2(centre.x - s.x / 2, centre.y - s.y / 2), text,
           Rgba(255, 255, 255, alpha), std::max(1.0f, size / 16), alpha);
  return r;
}

ImVec2 DeltaBadgeSize(float unit, int delta)
{
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), delta > 0 ? "+%d" : delta < 0 ? "–%d" : "±%d",
                delta < 0 ? -delta : delta);
  const ImVec2 s = Measure(LINE_SIZE * 1.15f * unit, buffer);
  return ImVec2(s.x + 2.4f * unit, s.y + 1.0f * unit);
}

ImVec2 DeltaBadge(ImDrawList* dl, float left, float centre_y, float unit, int delta, float alpha,
                  float scale)
{
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), delta > 0 ? "+%d" : delta < 0 ? "–%d" : "±%d",
                delta < 0 ? -delta : delta);
  const ImVec2 size = DeltaBadgeSize(unit, delta);
  const float k = scale > 0 ? scale : 1;
  const ImVec2 centre(left + size.x / 2, centre_y);
  const ImVec2 half(size.x * k / 2, size.y * k / 2);
  const ImVec2 min(centre.x - half.x, centre.y - half.y), max(centre.x + half.x, centre.y + half.y);
  const ImU32 fill = delta > 0 ? Rgba(46, 178, 84, alpha) :
                     delta < 0 ? Rgba(214, 58, 52, alpha) :
                                 Rgba(110, 116, 132, alpha);
  const float round = half.y;
  dl->AddRectFilled(ImVec2(min.x + 0.3f * unit, min.y + 0.4f * unit),
                    ImVec2(max.x + 0.3f * unit, max.y + 0.4f * unit), Rgba(0, 0, 0, alpha * 0.4f),
                    round);
  dl->AddRectFilled(min, max, fill, round);
  dl->AddRect(min, max, Rgba(255, 255, 255, alpha * 0.9f), round, ImDrawFlags_None,
              std::max(1.0f, 0.35f * unit * k));
  const float text_size = LINE_SIZE * 1.15f * unit * k;
  const ImVec2 ts = Measure(text_size, buffer);
  Outlined(dl, text_size, ImVec2(centre.x - ts.x / 2, centre.y - ts.y / 2), buffer,
           Rgba(255, 255, 255, alpha), std::max(1.0f, text_size / 16), alpha * 0.6f);
  return size;
}

ImVec2 Hint(ImDrawList* dl, float centre_x, float top, float unit, std::string_view text,
            float progress, float alpha)
{
  const float size = SMALL_SIZE * unit;
  const ImVec2 ts = Measure(size, text);
  const float pad_x = 2.4f * unit, pad_y = 0.9f * unit, bar = 0.5f * unit;
  const ImVec2 box(ts.x + 2 * pad_x, ts.y + 2 * pad_y + bar);
  const ImVec2 min(centre_x - box.x / 2, top), max(centre_x + box.x / 2, top + box.y);
  const float round = box.y / 2;
  dl->AddRectFilled(min, max, Rgba(12, 16, 32, alpha * 0.7f), round);
  dl->AddRect(min, max, Rgba(255, 255, 255, alpha * 0.25f), round, ImDrawFlags_None,
              std::max(1.0f, 0.25f * unit));
  Outlined(dl, size, ImVec2(centre_x - ts.x / 2, top + pad_y), text, Rgba(255, 255, 255, alpha),
           std::max(1.0f, size / 14), alpha);
  const float p = std::clamp(progress, 0.0f, 1.0f);
  if (p > 0)
  {
    // The hold fills left to right under the text.
    const float y = top + pad_y + ts.y + bar * 0.4f;
    const float x0 = centre_x - ts.x / 2;
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + ts.x, y + bar), Rgba(255, 255, 255, alpha * 0.2f),
                      bar / 2);
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + ts.x * p, y + bar),
                      Rgba(ColourOf(Tone::Warning), alpha), bar / 2);
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
