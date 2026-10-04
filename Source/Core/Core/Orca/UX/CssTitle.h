// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"

namespace Core
{
class CPUThreadGuard;
}

// The character select's mode title (ORCA.md "The character select's title"): the logo at the top
// left of the Versus character select, Brawl's BRAWL and Project+'s VERSUS, reads CASUAL, RANKED or
// FRIENDS in an online room, in the game's own place and look.
//
// It is a texture, not text: MenSelchrPanelRule3.1 (Brawl 132x48, Project+ 128x56, both C8 with an
// RGB5A3 palette) in the character select's archive (sc_selcharacter_en, MiscData 30's bres), one
// frame of the tab's pattern animation (3.2 is TEAM BATTLE, 3.3 SPECIAL BRAWL / VERSUS ...). Its
// pixels are the game's art, so Orca never draws into it: it marks the palette instead. The
// texture's background is one transparent palette entry (Brawl's entry 0, 0x0EEE; Project+'s entry
// 7, 0x0000); a mode moves that entry's blue by one to three steps, a colour nobody sees (its alpha
// is zero), which gives the texture another hash for each mode, and Orca's texture pack has a label
// for each of those (Data/Sys/Orca/Textures/RSBE01, Tools/orca/textures/make-labels.py): a session
// always loads the pack, as it does for the Online menu's labels. Without the pack the game's own
// title shows, unchanged to the eye.
//
// What it says, a pure function of emulated memory and the ports in effect at the boundary (the
// session's synced plan), so both machines write the same word at the same frame:
//   1. the match block's header (OnlineRules.h): a casual or ranked queue room, CASUAL or RANKED;
//   2. the main menu's pick Orca keeps for the back out (sqVsMelee +0x18, RSBE01.patches): a With
//      Friends button (24-27), FRIENDS;
//   3. another machine's player plugged in (two ports or more) with no queue header: a friends
//      room, FRIENDS (a friend who dropped into Group > Brawl, or Project+'s boot character
//      select);
//   4. the pick again: With Anyone's Casual (30) or Ranked (31), CASUAL or RANKED (the solo
//      character select a search waits on, before or after a room);
//   5. else the game's own title (Group > Brawl, Project+'s VERSUS, everything offline).
// No new state: the header and the pick are already in memory and travel with every snapshot and
// keyframe (a joiner's title is its host's). Friends rooms' game behaviour is untouched: the word
// is a colour the game only draws.
//
// Where (Brawl rev 2 and Project+, from harness RAM dumps): the scene manager (0x805A0060) +4 is
// the scene ("scSelctCharacter"), whose +0x410 is the character select's gfArchive; one of its
// words is the archive image ("ARC\0", named "sc_selcharacter_en" at +0x10). The bres's root group
// names the Palettes(NW4R) group, which names the PLT0. The manager's +0x10 is the running sequence
// ("sqVsMelee" while the Versus character select runs).
//
// The word is written only while it holds the game's value or one of the marks (anything else is
// not the texture this knows: left alone), only when it differs, at every frame boundary, first
// runs and re-runs alike, before anything is saved there. The archive is loaded fresh each time the
// character select starts, so a mark never outlives it; the game's title comes back by writing the
// game's value when nothing calls for a mark.
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::CssTitle
{
enum class Title : u8
{
  Game = 0,
  Casual = 1,
  Ranked = 2,
  Friends = 3,
};

std::string_view TitleName(Title title);

// The main menu's exit code Orca keeps in the running sqVsMelee (+0x18), 0 when the running
// sequence isn't sqVsMelee or the word isn't a menu code (the heap's fill, 0xCCCCCCCC, is not).
u32 ReadPick(const GuestMemory& memory);

// What the title should say (above). `players`: the ports plugged in at this boundary.
Title Wanted(const GuestMemory& memory, int players);

// The palette word that carries the mark, and the values it takes.
struct Slot
{
  u32 address = 0;  // 0: no title texture this knows (not on the character select, not loaded)
  u16 game = 0;     // the game's own value
  int step = 0;     // a mode's value is game + step * title (Brawl -1, Project+ +1)
  u16 ValueFor(Title title) const
  {
    return static_cast<u16>(game + step * static_cast<int>(title));
  }
  // The title a value stands for, if it is the game's or a mark.
  std::optional<Title> TitleOf(u16 value) const;
};
Slot FindSlot(const GuestMemory& memory);

// One boundary: writes the wanted title's value if the slot holds another of its values. Returns
// how many words were written (0 or 1); `shown` gets what the title says afterwards (Game when
// there is no slot).
int Apply(GuestMemory& memory, int players, Title* shown = nullptr);

// The frame hook's part (Brawl rev 2 and Project+ only), first runs and re-runs, with the ports in
// effect at the boundary.
void Frame(const Core::CPUThreadGuard& guard, bool resimulating,
           const std::vector<Orca::Events::PortInfo>& ports);
}  // namespace Orca::UX::CssTitle
