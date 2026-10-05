// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include <imgui.h>

// The overlay's building blocks for what Orca draws over the game's own screens (the set's end, the
// opponent's disconnect: SetEnd.h): a panel, a title, a line of text, a big number, a countdown,
// a rating's change and a hold hint. Everything about how they look is here and only here: since
// Orca 0.3.18 each is drawn with the kit (Kit.h), in the game's own fonts and the look of its
// menus (Brawl's or Project+'s), so the set's end reads as the game's own screen. The screens that
// use them only say what goes where, in overlay units (1% of the picture's height, OverlayUnit).
//
// Host-only drawing on ImGui's foreground list, over the presented picture: nothing here reads or
// writes the game.
namespace Orca::UX::Widgets
{
// What a piece says, which picks its colours.
enum class Tone : int
{
  Neutral,  // white on the dark plate
  Win,      // gold: VICTORY, "You win the set"
  Loss,     // red: DEFEAT, "This set counts as a loss"
  Warning,  // amber: the opponent disconnected, the countdown
};

// Sizes, in overlay units.
constexpr float TITLE_SIZE = 10.0f;   // VICTORY / DEFEAT
constexpr float HEADING_SIZE = 5.0f;  // "bo disconnected", a casual game's "YOU WON"
constexpr float LINE_SIZE = 3.4f;     // "2–0 vs bo", "They forfeit in"
constexpr float SMALL_SIZE = 2.8f;    // "Rating updating…"
constexpr float HINT_SIZE = 3.2f;     // "Hold Z to leave"
constexpr float NUMBER_SIZE = 6.5f;   // the rating
constexpr float COUNTDOWN_SIZE = 6.0f;  // the countdown's digits

// The size `text` takes at `size` pixels (a Line's).
ImVec2 Measure(float size, std::string_view text);
// The size a Title's capitals take (the game's heavy face), and a BigNumber's digits.
ImVec2 MeasureTitle(float unit, std::string_view text);
ImVec2 MeasureNumber(float unit, std::string_view digits);

// The plate behind a group of lines, from `min` to `max`: the look's panel (Brawl's rose plate,
// Project+'s teal slab).
void Panel(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone, float alpha);

// Big capitals (VICTORY, DEFEAT) centred on `centre_x` with their top at `top`, scaled about their
// centre by `scale` (an entrance or a pop). Returns their size at scale 1.
ImVec2 Title(ImDrawList* dl, float centre_x, float top, float unit, std::string_view text,
             Tone tone, float alpha, float scale = 1);

// A line of text `size` pixels tall, centred on `centre_x`, its top at `top`. Returns its size.
ImVec2 Line(ImDrawList* dl, float centre_x, float top, float size, std::string_view text, Tone tone,
            float alpha);

// A big number (a rating) centred on `centre_x`, its top at `top`, scaled about its centre. Returns
// its size at scale 1.
ImVec2 BigNumber(ImDrawList* dl, float centre_x, float top, float unit, int value, float alpha,
                 float scale = 1);

// A countdown centred on `centre`: the seconds left ("9") inside a ring that drains with
// `fraction` (of the whole wait, 1 full .. 0 empty). Returns the ring's outer radius.
float Countdown(ImDrawList* dl, ImVec2 centre, float unit, int seconds, float fraction, Tone tone,
                float alpha);
// The countdown's outer radius at `unit`, for layout.
float CountdownRadius(float unit);

// A rating's change, "+16" in green, "-16" in red, "+0" in grey: a badge with its left edge at
// `left` and its middle at `centre_y`, scaled about its centre. Returns its size at scale 1.
ImVec2 DeltaBadge(ImDrawList* dl, float left, float centre_y, float unit, int delta, float alpha,
                  float scale = 1);
// The badge's size at `unit`, for layout.
ImVec2 DeltaBadgeSize(float unit, int delta);

// A small hint ("Hold Z to leave") on a slim plate centred on `centre_x`, its top at `top`, with a
// bar under the text that fills with `progress` (0..1) while a button is held. Returns its size.
ImVec2 Hint(ImDrawList* dl, float centre_x, float top, float unit, std::string_view text,
            float progress, float alpha);

// "0:09", "1:05": a countdown's text.
std::string_view ClockText(int seconds, char (&buffer)[16]);
}  // namespace Orca::UX::Widgets
