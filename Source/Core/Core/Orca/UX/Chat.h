// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// The page's chat in fullscreen (ORCA.md "stdin", YouGame's ORCA_ONLINE_UX.md "Fullscreen chat").
// With the cap `chat` agreed, the YouGame page sends `chat <name> <text>` for a line someone else
// wrote in the room's chat while Orca fills the screen (the page's own chat can't be seen then),
// and Orca shows "<name>: <text>" as an overlay toast (Overlay.h, ShowChat). Each field is
// encodeURIComponent of UTF-8, so a message has no spaces or control characters on the wire and can
// never become a second command. The text is a player's own words: Orca never prints it to stdout,
// stderr or its log. Host-only drawing, never game memory: it can't desync.
namespace Orca::UX::Chat
{
// Code points once decoded (the page cuts to these, with "…").
inline constexpr std::size_t NAME_POINTS = 24;
inline constexpr std::size_t TEXT_POINTS = 140;
// Encoded: every code point at most 4 UTF-8 bytes, each "%XX" (the app's filter takes these too).
inline constexpr std::size_t NAME_ENCODED = NAME_POINTS * 12;
inline constexpr std::size_t TEXT_ENCODED = TEXT_POINTS * 12;

struct Line
{
  std::string name;
  std::string text;
};

// One field, percent-decoded. Encoded it holds only what encodeURIComponent leaves as it is
// (A-Z a-z 0-9 - _ . ! ~ * ' ( )) and "%XX", at most max_encoded characters; decoded it is valid
// UTF-8 (no overlong forms, surrogates, or code points past U+10FFFF) of 1 to max_points code
// points, with no control characters (C0, DEL, C1) and no line or paragraph separators. nullopt
// otherwise.
std::optional<std::string> DecodeField(std::string_view field, std::size_t max_encoded,
                                       std::size_t max_points);

// The whole stdin line, exactly "chat <name> <text>" (one space each). nullopt when it is
// malformed: the caller answers "unsupported chat", and says nothing more.
std::optional<Line> Parse(std::string_view line);
}  // namespace Orca::UX::Chat
