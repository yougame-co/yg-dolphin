// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include <imgui.h>

#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/YgOrb.h"

// Drawing the YouGame button and its lines (YgOrb.h) with the overlay's ImGui: only the overlay
// (Overlay.cpp) includes this, since the platform layer has no ImGui.
namespace Orca::UX::YgOrb
{
// The button's box, its dot and badge included; an empty box (min == max) when off.
Kit::Box ButtonBox(const Model::View& view);
// The button at its place with its ring, dot and badge (the video thread, in the overlay's ImGui
// frame).
void DrawButton(ImDrawList* dl, const Model::View& view);
// A row of the button's pill: a chat line (the name over the words, two lines at most), one of
// Orca's own toasts (its words beside YouGame's mark), or a notice (Current()'s: "<name> <text>" on
// one line, the Pill's mark on the avatar, the action's key and verb in grey at the pill's end).
struct PillRow
{
  enum class Type
  {
    Chat,
    Toast,
    Notice,
  };
  Type type = Type::Chat;
  std::string_view name;
  std::string_view text;
  std::string_view key;
  std::string_view verb;
  Kind kind = Kind::Join;  // a notice's
  float alpha = 1;
  double at = 0;  // when it came (the overlay's clock): the rows run oldest first
};
inline constexpr std::size_t PILL_ROWS = 3;

// The button's pill, the page's own (PlayOrb's Pill) drawn where Orca covers it: a slab that
// unrolls from under the button along its row at the top, with the newest PILL_ROWS of `rows` and
// Current()'s notices together, oldest first. Nothing when the button is off or no row shows.
// No allocation: the notices are copied out once per change (Model::Lines), words are cut to fit
// by measuring, never by building strings. Returns the slab (empty when none).
Kit::Box DrawPill(ImDrawList* dl, const Model::View& view, std::span<const PillRow> rows,
                  double now_ms);

// The longest start of `text` that `measure` (a width) keeps within `width`, at a UTF-8 character
// boundary, after a space when one fits in its second half (a word is never split unless it alone
// is too wide). Pure, for the tests.
template <typename Measure>
std::size_t FitPrefix(std::string_view text, float width, Measure&& measure)
{
  if (measure(text) <= width)
    return text.size();
  const auto continuation = [&](std::size_t i) {
    return i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80;
  };
  // The start of `lo` bytes fits, that of `hi` does not; both at character boundaries.
  std::size_t lo = 0, hi = text.size();
  while (true)
  {
    std::size_t mid = lo + (hi - lo) / 2;
    while (mid > lo && continuation(mid))
      --mid;
    if (mid == lo)
    {
      mid = lo + 1;
      while (mid < hi && continuation(mid))
        ++mid;
    }
    if (mid >= hi)
      break;
    if (measure(text.substr(0, mid)) <= width)
      lo = mid;
    else
      hi = mid;
  }
  const std::size_t space = text.substr(0, lo).rfind(' ');
  if (space != std::string_view::npos && space > lo / 2)
    return space;
  return lo;
}
}  // namespace Orca::UX::YgOrb
