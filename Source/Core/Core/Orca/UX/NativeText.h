// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"

namespace Core
{
class CPUThreadGuard;
}

// Orca's words in the game's own text boxes (ORCA.md "Native text"; Luis: Orca's UI must look
// native). Brawl draws its menu text from message buffers: a window formats a message into a buffer
// of its own once (its set-up codes, then the message's bytes, UTF-8 with inline codes), then draws
// that buffer every frame until the game formats another message there. A buffer Orca rewrites is
// drawn in the game's font, size, colour and place from the next frame on, and stays until the
// game formats that window again (proven under the harness, 2026-10-04: one write, still on screen
// 150 frames later).
//
// The message buffer (Brawl rev 2, from harness RAM dumps of the character select, the stage select
// and the results screen): an object whose first word is its vtable 0x804540E0, +0x28 the buffer's
// capacity, +0x2C the length in use, +0x30 the buffer; the byte after the length is 0x01 (the end).
// The buffer starts with the window's set-up, 0x17 ..., which ends with its colours, 0x02 n 0x0C
// r g b a, 0x04 r g b a, then 0x03 0x00 0x00. The message follows: printable UTF-8 between inline
// codes, 0x11 + 4 bytes (a width code: without the rules bar's, a longer line is not drawn at all)
// and 0x12 0x02 0x0C r g b a (a colour) before the words, 0x13 (the colour's end) and 0x11 + 4 after.
// Orca replaces only the words: the set-up and every code stay the game's, so the text keeps the
// game's colour, size and fit (a longer line is squeezed to the box's width, as the game's own
// are). The font has ASCII and Latin-1 letters but no middle dot or dash: the words are ASCII.
//
// Two places on the Versus character select (muSelCharTask, scene +0x400):
//   - each panel's name plate ("PLAYER 1", "NONE"): player area i (task +0x44 + 4i, whose +0x1B0
//     is i) +0x174 -> its message buffer. A named player's plate reads their YouGame name, in
//     capitals like the game's plates, and nothing else (Luis, 2026-10-05: no rating by the name).
//     On the queue's own select, while it searches, the opponent's empty seat reads SEARCHING
//     (the game's NONE again when it stops).
//   - the rules bar at the top, right of the mode title ("3-man survival test!", after the stock
//     count): task +0x538 -> its message buffer. In a queue room (and on the queue's own character
//     select) it says what happens now, in the third person, with the timer: "Game 1: ADA locked
//     in, BO to press Start  0:24", "ADA 1-0 BO: BO picks, A then Start  0:45". While READY TO
//     FIGHT! is up and relabelled (Relabel::ClockInBand) the timer is the band's, not the bar's.
// Both are a pure function of emulated memory (the match block's header, the queue's ready flags
// and timer, the ranked set, the character order, the tokens) and the session's synced ports
// (names): the same bytes on both machines at the same frame, first runs and re-runs
// alike, written only where they differ, before the session saves the boundary. Never "you": one
// text for both players. The game formats a plate again when a port joins, leaves or picks a tag;
// the next boundary puts the name back (one frame of the game's own words).
//
// Not here (ORCA.md "Native text" has the survey): the stage select has no text box on screen (its
// one window is the Stage Builder name, hidden); the results screen's (the winner's name, the mode
// and rule panel) are proven to take text but not yet written; VICTORY/DEFEAT and the rating change
// differ per player or come from the server, so they stay the overlay's.
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::NativeText
{
constexpr u32 MESSAGE_BUFFER_VTABLE = 0x804540E0;
constexpr u32 BUFFER_CAPACITY = 0x28;
constexpr u32 BUFFER_LENGTH = 0x2C;
constexpr u32 BUFFER_DATA = 0x30;
constexpr u8 END = 0x01;

// One message buffer's layout at a boundary: where the words are, and what is around them.
struct Box
{
  u32 object = 0;  // 0: not a message buffer this knows
  u32 buffer = 0;
  u32 capacity = 0;
  u32 length = 0;
  u32 words = 0;      // offset of the words' first byte (after the set-up and the leading codes)
  u32 words_end = 0;  // offset of the first code after them (the trailing codes, or the end)
};
// Reads the buffer `object` points at (validated: the vtable, sizes, the set-up's shape, the end
// byte). nullopt for anything else.
std::optional<Box> ReadBox(const GuestMemory& memory, u32 object);
// The words the box shows now.
std::string Words(const GuestMemory& memory, const Box& box);
// Puts `text` in place of the box's words, keeping the set-up and the codes around them; only the
// bytes that differ. `text` must be printable ASCII (Ascii()). Returns 1 if anything was written, 0
// if it already said that or it would not fit.
int SetWords(GuestMemory& memory, const Box& box, std::string_view text);

// Text as the font can draw it: printable ASCII kept; the overlay's "·" and "–" become "-", "…"
// becomes "...", anything else outside ASCII is dropped; at most `max` bytes.
std::string Ascii(std::string_view text, size_t max = 96);
// A YouGame username on a name plate: capitals, as Ascii(), at most 10 characters.
std::string PlateName(std::string_view username);
// "m:ss" of `frames` (60 a second), rounded up.
std::string Clock(int frames);

// What the character select's boxes should say at this boundary; empty: leave the game's words.
struct CssWords
{
  bool on_css = false;
  std::array<std::string, 4> plates;
  std::string rules;
};
// `band_clock`: Relabel::ClockInBand for this boundary, when the caller has it (the frame hook:
// Relabel's own Apply just computed it), else computed here.
CssWords Wanted(const GuestMemory& memory, const std::vector<Events::PortInfo>& ports,
                std::optional<bool> band_clock = std::nullopt);

// The boxes themselves (nullopt where the character select has none this knows).
struct CssBoxes
{
  std::array<std::optional<Box>, 4> plates;
  std::optional<Box> rules;
};
CssBoxes FindCssBoxes(const GuestMemory& memory);

// One boundary's writes. Returns how many boxes were written; `shown` gets which boxes now show
// Orca's words (bit p a plate, bit 4 the rules bar). `rules_bar` false leaves the bar the game's
// (Project+'s: its message has no width code, so a long line isn't drawn).
int Apply(GuestMemory& memory, const std::vector<Events::PortInfo>& ports, u8* shown = nullptr,
          bool rules_bar = true, std::optional<bool> band_clock = std::nullopt);
constexpr u8 SHOWN_RULES = 0x10;

// The frame hook's part (Brawl rev 2: the plates and the rules bar; Project+: the plates), first
// runs and re-runs. On first runs it also tells the
// overlay which of its lines the game now shows (Shown()).
void Frame(const Core::CPUThreadGuard& guard, bool resimulating,
           const std::vector<Events::PortInfo>& ports, std::optional<bool> band_clock = std::nullopt);
// What the last first-run boundary showed (Apply's `shown`), for the overlay: it drops a port's
// label over a plate that names the player, and its top lines over a rules bar that says them.
u8 Shown();
}  // namespace Orca::UX::NativeText
