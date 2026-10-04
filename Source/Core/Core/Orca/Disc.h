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

// Whether the file Orca was told to boot (-e) is there and opens, checked before anything else:
// "disc_missing" when nothing is at the path (a disc image moved or deleted since the app stored
// it, its drive unplugged), "disc_unreadable" when it is there but won't open. The sentence is the
// path. Dolphin's own check would raise a panic alert instead, a message box nobody sees in the
// app's window on Windows, and the run would wait on it forever.
DiscCheck CheckBootFile(const std::string& path);
}  // namespace Orca
