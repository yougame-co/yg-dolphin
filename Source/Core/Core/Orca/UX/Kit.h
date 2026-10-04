// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

// Orca's overlay kit: the few shapes every element Orca adds to the screen is drawn with, so they
// read as part of Brawl's and Project+'s own menus instead of text over the picture
// (Luis, 2026-10-04: "integrated into the games UI itself ... SEAMLESS").
//
//   - Text in the game's own fonts, read at run time from the player's disc (`system/font/`,
//     GameFont.h): Body is the menus' rounded sans (font_latin1), Heavy the game's heavy display
//     face (font_hira). Kept in memory only; without the disc (or before it is read) the text falls
//     back to ImGui's font with the same colours, outline and shadow.
//   - Plate: the game's panel, a rounded slab with a vertical gradient, a light rim inside a dark
//     edge, a gloss over its top half and a drop shadow.
//   - Banner: a band across the picture with cut ends, a bright line along each edge and a title
//     in the heavy face (the "READY TO FIGHT" band's shape).
//   - Title: the heavy face in italics, white into the menus' pale blue, outlined, with a shadow.
//   - Label: a line in the body face, white, outlined, for everything smaller.
//   - Timer: "0:24" in the heavy face on a small plate, gold under 10 s and red, pulsing, under 5.
//   - Badge: a pill in a port's colour with a white rim ("LOCKED IN", a name tab).
//   - Pointer: a player's cursor, a ring in their colour with ticks and their tab, a gold ring
//     round it on their turn; Frame: a tile framed in a player's colour with their tab.
//
// Everything is drawn with the host's ImGui over the presented picture: local display only, never
// read by emulation. Sizes are in pixels, `unit` is OverlayUnit's 1% of the picture's height.
namespace Orca::UX::GameFont
{
struct Atlas;
}

namespace Orca::UX::Kit
{
enum class Face
{
  Body,
  Heavy,
};

enum class Align
{
  Left,
  Centre,
  Right,
};

// The colours the kit draws with. A port's tone is that port's panel colour (PortColourOf).
enum class Tone
{
  Navy,  // the menus' dark blue panels
  Gold,  // agreement, the turn's player, a win
  Grey,  // inactive, a hint
  Red,   // port 1, a loss, time running out
  Blue,  // port 2
  Yellow,
  Green,
};
Tone PortTone(int port);

struct TextLook
{
  Face face = Face::Body;
  float px = 20;  // the line's height (the font's cell), pixels
  ImU32 top = IM_COL32_WHITE;
  ImU32 bottom = IM_COL32_WHITE;
  float outline = 0;  // pixels; 0: none
  ImU32 outline_colour = IM_COL32(12, 14, 24, 255);
  ImVec2 shadow{0, 0};  // offset in pixels; (0, 0): none
  ImU32 shadow_colour = IM_COL32(0, 0, 0, 150);
  float italic = 0;    // the top's shift to the right per pixel of height
  float tracking = 0;  // pixels added after every glyph
};

// The body face's look at a size: white, outlined, a small shadow.
TextLook LabelLook(float unit, float size = 3.0f);
// The heavy face's: italic, white into pale blue, outlined, shadowed.
TextLook TitleLook(float unit, float size = 5.0f);

// The disc the game runs from (the boot's disc, or a launcher's default disc). The kit reads the
// fonts from it once, on a thread of its own, the first time it draws. Any thread, before Init.
void SetGameDisc(std::string path);
// Each overlay frame, before drawing (the video thread, in the ImGui frame): uploads the fonts once
// they are read.
void BeginFrame();
// Waits for the font reader (the process is going).
void Shutdown();
// Whether text in `face` is drawn in the game's font yet.
bool HasGameFont(Face face);

ImVec2 Measure(const TextLook& look, std::string_view text);
// Draws `text` with its top-left at `pos` (moved for `align`); returns its size.
ImVec2 Text(ImDrawList* dl, ImVec2 pos, const TextLook& look, std::string_view text,
            float alpha = 1, Align align = Align::Left);

// `text` broken at spaces into lines no wider than `width` (a word wider than that on a line of its
// own).
std::vector<std::string> Wrap(const TextLook& look, std::string_view text, float width);

struct Box
{
  ImVec2 min, max;
};

// The game's panel over [min, max] in `tone`.
void Plate(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone, float alpha = 1);
// A band across [left, right] whose top is at `top`, `height` tall, with `title` centred in it
// (and `note` under the title in the body face, when not empty). Returns the band.
Box Banner(ImDrawList* dl, float left, float right, float top, float height, float unit,
           Tone tone, std::string_view title, std::string_view note = {}, float alpha = 1);
// The heavy italic title, top-left at `pos` (moved for `align`). Returns its box.
Box Title(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float size = 5.0f,
          Align align = Align::Left, float alpha = 1);
// A line in the body face, `colour` its fill (white by default). Returns its box.
Box Label(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float size = 3.0f,
          Align align = Align::Left, ImU32 colour = IM_COL32_WHITE, float alpha = 1);
// A line of label text on a navy plate, padded: the toasts', the chat's, the hints'. Returns the
// plate.
Box LabelPlate(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float size = 3.0f,
               Align align = Align::Left, Tone tone = Tone::Navy, float alpha = 1);
// "m:ss" for `seconds` on its plate, its centre at `centre`. `now_s` paces the last seconds' pulse
// (ImGui's clock). Returns the plate.
Box Timer(ImDrawList* dl, ImVec2 centre, float unit, int seconds, double now_s, float alpha = 1);
// A pill in `tone` with `text` in white capitals, centred on `centre`. Returns the pill.
Box Badge(ImDrawList* dl, ImVec2 centre, float unit, std::string_view text, Tone tone,
          float size = 3.6f, float alpha = 1);
// A player's cursor centred on `at`, `radius` pixels, in their port's colour, with `tag` on a tab
// to its lower left (`tag_left`) or right; a gold ring round it on their turn.
void Pointer(ImDrawList* dl, ImVec2 at, float radius, float unit, int port, std::string_view tag,
             bool turn, bool tag_left, double now_s);
// A tile [min, max] framed in `tone` with `tag` on a tab on its top edge (left or right end).
void Frame(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone, std::string_view tag,
           bool tag_left, float inset = 0);

// Tests only: text in `face` drawn with `atlas` as if it were on the GPU as `texture` (null: back to
// the GPU's). The atlas must outlive its use.
void UseAtlasForTests(Face face, const GameFont::Atlas* atlas, ImTextureID texture);

// ORCA_UX_KIT_DEMO (design checks only): 1, every primitive over the picture (DrawDemo) in place
// of the overlay; 2, the overlay's own elements drawn with the kit (Overlay.cpp); 0 (unset): off.
int DemoMode();
void DrawDemo(ImDrawList* dl, ImVec2 picture_min, ImVec2 picture_size, float unit, double now_s);
}  // namespace Orca::UX::Kit
