// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

// Player-visible names say YG Baiji (called Orca until 0.3.39); upstream attribution and licence
// text are unchanged. The code, its files and its protocols keep the name Orca (ORCA.md).
namespace Orca
{
// The name players see.
inline constexpr std::string_view PRODUCT_NAME = "YG Baiji";

// The window title in a session: "YG Baiji — <game> — <room status>".
std::string WindowTitle();

// Replaces the whole words "Dolphin" and "Orca" with PRODUCT_NAME. A whole word has no letter,
// digit, '_', '/' or '\' right before or after it, no '.' right before it, and no '.' followed by
// a letter or digit right after it: "Dolphin's" and "restart Dolphin." change, "DolphinNoGUI",
// paths and file names ("Orca.app", "/Dolphin/") don't. Idempotent: PRODUCT_NAME contains neither
// word.
std::string BrandWords(std::string_view text);

// BrandWords in player-visible text during a session; otherwise the text unchanged.
std::string BrandText(std::string_view text);
}  // namespace Orca
