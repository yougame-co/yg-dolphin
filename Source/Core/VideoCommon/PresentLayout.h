// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "Common/MathUtil.h"

// Orca (embed `view`, ORCA.md "Embedding"): the box the game's picture is laid out in when it is
// not the window. The YouGame page opens its play menu over the left of the player box; nothing in
// the page can draw over Orca's window, so the page cuts the window (`rect`) to start right of the
// panel and keeps the picture laid out in the whole box (`view`): the game neither shrinks nor
// moves, the panel covers its left part. Pure, so the tests drive it.
namespace VideoCommon
{
// The layout box in the window's own units (`rect`'s: physical pixels on Windows, backing pixels on
// macOS), relative to the window's top left, and the window's size in those units.
struct LayoutHint
{
  int win_w = 1;
  int win_h = 1;
  int x = 0;
  int y = 0;
  int w = 1;
  int h = 1;

  constexpr bool operator==(const LayoutHint&) const = default;
};

// A box in backbuffer pixels (it may start left of or above the backbuffer and run past it).
struct LayoutBox
{
  float x = 0;
  float y = 0;
  float w = 0;
  float h = 0;

  constexpr bool operator==(const LayoutBox&) const = default;
};

// How far the backbuffer's scale over the window may differ across and down for a hint still to be
// for this backbuffer: macOS rounds a window of whole backing pixels to points and back.
constexpr float LAYOUT_SHAPE_TOLERANCE = 0.02f;

// `hint`'s box in a backbuffer of bb_w x bb_h: scaled by the backbuffer's size over the window's
// (the units differ by the backing scale, and by rounding). nullopt when the hint is for a window
// of another shape (the backbuffer hasn't followed a resize yet) or anything is empty.
std::optional<LayoutBox> LayoutBoxIn(const LayoutHint& hint, int bb_w, int bb_h);

// The layout a present uses: the box, and the hint it came from (none: the window).
struct LayoutChoice
{
  LayoutBox box;
  std::optional<LayoutHint> hint;
};

// The box the picture is laid out in now: `latest`'s when it is for this backbuffer, else
// `applied`'s, the one the last present used (the window was just resized and the backbuffer still
// has the old size, for a frame or two), else the whole backbuffer. No `latest` (no `view`, or
// `view off`): the whole backbuffer, the layout following the window as Dolphin's does.
LayoutChoice ChooseLayout(const std::optional<LayoutHint>& latest,
                          const std::optional<LayoutHint>& applied, int bb_w, int bb_h);

// The picture, draw_w x draw_h whole pixels, centred in `box` as Dolphin centres it in the window.
// It can run past the backbuffer: Present() crops it (AdjustRectanglesToFitBounds).
MathUtil::Rectangle<int> PlaceInBox(const LayoutBox& box, int draw_w, int draw_h);

// Whether any of the placed picture is in the backbuffer. With a `view` it can be none of it (a
// window cut to the view's pillarbox bar): cropping it to the backbuffer would then give an
// inverted rectangle, so Present() draws no picture (the backbuffer is cleared to black).
bool PictureInBackbuffer(const MathUtil::Rectangle<int>& picture, int bb_w, int bb_h);
}  // namespace VideoCommon
