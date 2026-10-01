// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace Orca
{
// The one game Orca.app plays for now, and how the disc picker asks for it.
inline constexpr std::string_view APP_GAME_ID = "RSBE01";
inline constexpr std::string_view APP_DISC_PROMPT =
    "Choose your own Super Smash Bros. Brawl disc image (USA, Rev 2).";

struct DiscCheck
{
  bool ok = false;
  // The status code for a refusal (Orca::Status), and a sentence for the player.
  std::string code;
  std::string sentence;
};

// Whether the disc image at path is the game (by ID) at the revision its Orca profile names,
// checked before boot.
DiscCheck CheckDisc(const std::string& path, std::string_view game_id);
}  // namespace Orca
