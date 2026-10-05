// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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
// Current()'s lines as a column, each card's top left at `pos.x` and the first's at `pos.y`, at
// most `width` wide and none starting past `bottom` (the video thread: each line is laid out once,
// and again only when the unit or the width changes). Returns the y under the last.
float DrawLines(ImDrawList* dl, const Model::View& view, ImVec2 pos, float unit, float width,
                float bottom, double now_ms);
}  // namespace Orca::UX::YgOrb
