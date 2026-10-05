// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

// The game's own fonts, for the overlay's kit (Kit.h): Brawl's NW4R fonts (RFNT: a glyph sheet
// in a GX texture format, a width table and character maps), parsed from the bytes the player's
// own disc holds (`system/font/`, LZ77 type 0x10 for the `.arc` ones) and turned into an atlas of
// the glyphs the overlay writes. Nothing here is a copy of the game's art: it is read at run time,
// kept in memory only, and never written anywhere.
namespace Orca::UX::GameFont
{
// Nintendo's LZ77 (type 0x10): a 4-byte header (0x10, the size, 24-bit little endian), then
// blocks of a flag byte and eight literals or back references. Nullopt when malformed.
std::optional<std::vector<u8>> DecompressLz10(std::span<const u8> data);

// One UTF-8 code point at `*at` (advanced past it); U+FFFD for a malformed byte.
char32_t NextCodePoint(std::string_view text, std::size_t* at);

struct Widths
{
  int left = 0;     // where the glyph's picture starts, from the pen
  int glyph = 0;    // the picture's width
  int advance = 0;  // how far the pen moves
};

class Rfnt
{
public:
  // Nullopt (and why in `error`) for anything that isn't an RFNT this parser can read: version
  // 1.4 (0x0104) as the Wii's fonts are, sheets in I4, I8, IA4 or IA8.
  static std::optional<Rfnt> Parse(std::vector<u8> data, std::string* error = nullptr);

  int CellWidth() const { return m_cell_w; }
  int CellHeight() const { return m_cell_h; }
  int Ascent() const { return m_ascent; }
  int LineFeed() const { return m_linefeed; }
  int Baseline() const { return m_baseline; }
  std::size_t GlyphCount() const { return m_glyphs; }
  // The glyph index for a code point, nullopt when the font has none (never the font's
  // stand-in glyph: the caller picks its own substitute).
  std::optional<u16> IndexOf(char32_t code) const;
  Widths WidthsOf(u16 index) const;
  // The glyph's coverage (0-255), CellWidth() x CellHeight(), row major. False when the index is
  // past the sheets.
  bool GlyphAlpha(u16 index, std::vector<u8>* out) const;

private:
  struct Map
  {
    u16 begin, end, method;
    std::size_t info;  // offset of the map's table
  };
  struct WidthBlock
  {
    u16 begin, end;
    std::size_t table;
  };
  u16 ReadU16(std::size_t at) const;
  const std::vector<u8>& Sheet(int sheet) const;

  std::vector<u8> m_data;
  int m_cell_w = 0, m_cell_h = 0, m_ascent = 0, m_linefeed = 0, m_baseline = 0;
  u16 m_alter = 0;
  Widths m_default;
  std::size_t m_glyphs = 0;
  u32 m_sheet_size = 0;
  int m_sheet_count = 0, m_sheet_format = 0, m_per_row = 0, m_per_column = 0;
  int m_sheet_w = 0, m_sheet_h = 0;
  std::size_t m_sheet_data = 0;
  std::vector<Map> m_maps;
  std::vector<WidthBlock> m_widths;
  mutable std::map<int, std::vector<u8>> m_sheets;  // decoded coverage, sheet by sheet
};

// The fonts at `files` (paths in the game partition, e.g. "system/font/font_latin1.arc") read from
// the disc image at `disc`, unpacked when LZ77; one entry per file, nullopt for one it couldn't read
// (and why in `error`). Nothing when the disc isn't `game_id`. Any thread: opens its own volume.
std::vector<std::optional<Rfnt>> ReadFromDisc(const std::string& disc,
                                              std::span<const char* const> files,
                                              std::string_view game_id, std::string* error = nullptr);

// The glyphs the overlay writes, packed: white with the coverage as alpha (RGBA8), with mip
// levels, so the GPU's own filtering draws them at any size.
struct Atlas
{
  struct Glyph
  {
    float u0, v0, u1, v1;    // the cell's picture in the atlas
    float left, width;       // font pixels, from the pen
    float advance;           // font pixels
  };
  int width = 0, height = 0;
  std::vector<std::vector<u8>> levels;  // level n is (width >> n) x (height >> n)
  std::map<char32_t, Glyph> glyphs;
  float cell_height = 0;  // font pixels: a line's height and every glyph's quad
  float ascent = 0;
  float space = 0;  // a space's advance
};

// The code points every overlay line can use: ASCII, Latin-1 and the punctuation the overlay's
// lines are written with (· – — … ’ “ ” • ← ↑ → ↓ ×).
std::vector<char32_t> OverlayCodePoints();
// What to draw for a code point the font lacks (the first the font has), e.g. the middle dot for a
// heavy face without one; empty: nothing.
std::u32string Substitute(char32_t code);

// Every code point of `codes` the font has, each in its own cell with `pad` empty pixels round it
// (so filtering never reaches a neighbour), `levels` mip levels (at least 1).
Atlas BuildAtlas(const Rfnt& font, std::span<const char32_t> codes, int pad = 4, int levels = 4);
}  // namespace Orca::UX::GameFont
