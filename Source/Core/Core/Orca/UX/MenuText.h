// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

// The main menu's own words about going online, rewritten so none of them is Nintendo's (ORCA.md
// "Online menu"; Luis: nothing about connecting may look like Nintendo's). Brawl's footer under
// the Online button says "Play different modes while connected to Nintendo WFC.", Project+'s
// "Play different modes online. (Discontinued)"; both become "Play different modes online.", the
// With Anyone page's descriptions fit Casual and Ranked, and the With Friends page's say what each
// of its buttons now does (the local Versus character select, where friends drop in) instead of
// promising a roster or modes that aren't there. The textures (the button's label, Brawl's
// "NINTENDO WFC" title, Casual and Ranked) are Orca's texture pack, which never touches emulated
// memory; these words are text the game prints, so they are rewritten where the game keeps them.
//
// Where (Brawl rev 2 and Project+, found under the harness and in the disc's and the SD card's
// mu_menumain archive): the scene manager (0x805A0060) +4 is the main menu's scene ("muMenuMain"),
// whose +0xAC0 is its gfArchive, which points at the decompressed archive image ("ARC\0", named
// "mu_menumain_en" at +0x10); its MiscData entries 5 (the menu's descriptions) and 10 (the
// records' descriptions) are message files: a table of u32 offsets, message k from offset k to
// offset k+1, single-byte text with inline control codes (0x12 0x02 0x0C r g b a: a colour,
// 0x13: its end). The game prints a message from there each time it shows it.
//
// A message is rewritten only while its bytes are exactly the game's own, into the same slot (never
// longer; the rest is zeros, which end the text), so it lands once per load of the archive and
// never anywhere else. A pure function of emulated memory, applied at every frame boundary on
// both machines, first runs and re-runs alike: written before anything is saved there, so
// snapshots and keyframes hold it. UX kCompatVersion covers it.
//
// The description box at the bottom of the menu prints from a copy: when a page shows a message,
// the box formats it (control codes, then the message's whole slot) into a buffer of its own and
// draws that buffer every frame. A menu that opens straight on a page (B out of an Online
// character select comes back to With Friends or With Anyone) loads the archive and formats that
// page's first description in the same frame, before any boundary could rewrite the message, so
// the box's copy is rewritten too, the same way: while it holds a rewritten message's original
// bytes, and the panel says that message is the one shown. Found under the harness (Brawl rev 2
// and Project+ alike): muMenuMain +0x7BC is the description panel, whose +0x40 is the message shown
// and +0x48 its window; the window's +0x4 is the archive's MiscData 5 and +0x8 -> +0x9C its text,
// with the buffer at +0x50, its length at +0x4C and its capacity at +0x48. The bytes written are
// the ones the game formats from the rewritten message itself.
namespace Orca::UX
{
class GuestMemory;

struct MenuTextEdit
{
  u32 file;                   // the archive's MiscData index
  u32 message;                // the message within it
  std::string_view original;  // the slot's bytes as the game ships them
  std::string_view text;      // what it becomes (at most as long; zeros after)
};

// The edits for both games (Brawl's and Project+'s originals differ, each edit matches its own).
std::span<const MenuTextEdit> MenuTextEdits();

// The main menu's archive image, if the main menu is up and its archive is loaded; 0 otherwise.
u32 FindMenuArchive(const GuestMemory& memory);
// The address of MiscData `file`'s data in that image, 0 if it has none.
u32 FindArchiveFile(const GuestMemory& memory, u32 archive, u32 file);
// The menu descriptions' message file (MiscData 5), where the box's messages come from.
constexpr u32 DESCRIPTION_FILE = 5;

// The description box's copy of the message it shows, if the main menu's panel is up and its window
// prints from `messages` (the archive's MiscData 5); a zero buffer otherwise.
struct MenuDescription
{
  u32 buffer = 0;
  u32 length = 0;
  u32 message = 0;  // the message shown
};
MenuDescription FindMenuDescription(const GuestMemory& memory, u32 messages);
// Rewrites the box's copy of an edit's original (at most one: the box shows one message). Returns
// 1 if it did.
int ApplyMenuDescription(GuestMemory& memory, u32 messages, std::span<const MenuTextEdit> edits);
// One boundary's writes: the messages, then the box's copy. Returns how many were rewritten, and in
// `in_box` how many of them were the box's copy.
int ApplyMenuText(GuestMemory& memory, std::span<const MenuTextEdit> edits,
                  int* in_box = nullptr);

// The frame hook's part (Brawl rev 2 and Project+ only).
void WriteMenuText(const Core::CPUThreadGuard& guard);
}  // namespace Orca::UX
