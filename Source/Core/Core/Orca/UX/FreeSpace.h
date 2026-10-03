// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "Common/CommonTypes.h"

// Emulated memory that Orca may take for its own data and code in Brawl (RSBE01 rev 2) and
// Project+ v3.2 (ORCA.md "Free space"). Nothing reads or writes these addresses yet: they are the
// RAM and code-space spike's answer for the online rules' match block and the lock patches' code
// caves.
//
// All of it is dead code in sora_scene.rel (module 1), at the same address in both games: the
// methods of Brawl's online sequences sqNetAnyOkiraku (With Anyone's Basic Brawl),
// sqNetFriendList, sqNetFriendMelee, sqNetFriendKumite, sqNetFriendHomerun, sqNetAnyTeamMelee and
// sqNetAnyWatch.
// - sora_scene is resident: the game loads it once at boot (Project+ ships no copy of its own).
// - Those sequences start only from sqMenuMain::setNext's entries for exit codes 24-27 and 29-31,
//   which Orca's patches send to its own block instead (ORCA.md "Online menu"). Every other
//   reference into these runs comes from the sequences' own vtables and state tables, which only
//   their own code reads; no other module of the disc or of Project+ refers to them (checked from
//   the RELs' relocations).
// - Not free inside or between the runs:
//   - each sequence's create function (kSequenceCreates), which runs at boot when sora_scene
//     registers its sequences;
//   - the words the loader relocates against sora_melee (module 27, kSoraMeleeRelocations): they
//     hold the game's values while sora_melee is loaded, and the loader would rewrite them if it
//     ever relinked that module;
//   - 0x806F3DA4, which Project+'s codeset nops ("Online Handicap Disable"): leaving it out keeps
//     every cave word the same in both games.
// - Proven under the harness, in both games: unchanged from boot through the main menu, both
//   Online picks and the back out, character and stage select, two whole matches and their
//   results; filled with illegal instructions right after boot, the same runs reach the same scenes
//   with every other byte of RAM the same (ORCA.md "Free space").
//
// Before Orca writes them these addresses hold the game's dead code (the block's first word is
// kMatchBlockOriginalWord), never zeros: a reader of the block checks its magic. A cave is written
// as a guarded group of Data/Sys/Orca/<profile>.patches whose originals are the code that is there
// (GamePatches.h), so it lands once per boot, the same in both games. The block is data only:
// written by Orca's frame hook (a pure function of memory on both machines), never by a patch
// group, and 32-byte aligned so it shares no cache line with a cave. Rollback snapshots and drop-in
// keyframes carry it, like the rest of MEM1.
namespace Orca::UX::FreeSpace
{
struct Range
{
  u32 begin = 0;
  u32 end = 0;  // exclusive

  constexpr u32 Size() const { return end - begin; }
  constexpr bool Contains(u32 address) const { return address >= begin && address < end; }
  constexpr bool Overlaps(const Range& other) const
  {
    return begin < other.end && other.begin < end;
  }
};

// sora_scene.rel as the game loads it (both games): the module at 0x806BB480, its text from +0xD4.
constexpr u32 kSoraSceneBase = 0x806BB480;
constexpr Range kSoraSceneText{0x806BB554, 0x806F8ADC};

// The match block: the header and the set state, 512 bytes (the design needs about 96), in
// sqNetAnyOkiraku::setNext. It can grow to kMatchBlockLimit, the next relocated word.
constexpr Range kMatchBlock{0x806F2380, 0x806F2580};
constexpr u32 kMatchBlockLimit = 0x806F2618;
constexpr u32 kMatchBlockOriginalWord = 0x3A200000;  // `li r17, 0`, at kMatchBlock.begin

// Code caves, 16,908 bytes in all: runs of 500 bytes or more with no relocated word in them (the
// shorter runs between relocated words are left out). Every MEM1 address is a relative branch
// (`b`/`bl`, +-32 MB) away.
constexpr std::array<Range, 12> kCodeCaves{{
    {0x806F2674, 0x806F2D24},  // sqNetAnyOkiraku::setNext, its states and exit (1,712 bytes)
    {0x806F2D74, 0x806F30E0},  // sqNetFriendList (876)
    {0x806F31F0, 0x806F34CC},  // sqNetFriendMelee (732)
    {0x806F352C, 0x806F3DA4},  // sqNetFriendMelee, up to Project+'s nop (2,168)
    {0x806F3DA8, 0x806F3FA0},  // sqNetFriendMelee, after it (504)
    {0x806F3FF0, 0x806F4468},  // sqNetFriendKumite (1,144)
    {0x806F45B0, 0x806F4C70},  // sqNetFriendKumite (1,728)
    {0x806F4CC0, 0x806F5180},  // sqNetFriendHomerun (1,216)
    {0x806F52E0, 0x806F58B4},  // sqNetFriendHomerun (1,492)
    {0x806F6C74, 0x806F7220},  // sqNetAnyTeamMelee (1,452)
    {0x806F7328, 0x806F76F8},  // sqNetAnyTeamMelee (976)
    {0x806F7748, 0x806F82A4},  // sqNetAnyWatch (2,908)
}};

// Each online sequence's create function: runs at boot.
constexpr std::array<Range, 7> kSequenceCreates{{
    {0x806F229C, 0x806F22EC},  // sqNetAnyOkiraku
    {0x806F2D24, 0x806F2D74},  // sqNetFriendList
    {0x806F30E0, 0x806F3130},  // sqNetFriendMelee
    {0x806F3FA0, 0x806F3FF0},  // sqNetFriendKumite
    {0x806F4C70, 0x806F4CC0},  // sqNetFriendHomerun
    {0x806F6C24, 0x806F6C74},  // sqNetAnyTeamMelee
    {0x806F76F8, 0x806F7748},  // sqNetAnyWatch
}};

// The words in those sequences that the loader relocates against sora_melee (module 27).
constexpr std::array<u32, 25> kSoraMeleeRelocations{
    0x806F2368, 0x806F2618, 0x806F2634, 0x806F2654, 0x806F2670, 0x806F31EC, 0x806F34CC,
    0x806F34E8, 0x806F350C, 0x806F3528, 0x806F4468, 0x806F4550, 0x806F456C, 0x806F4590,
    0x806F45AC, 0x806F5180, 0x806F5280, 0x806F529C, 0x806F52C0, 0x806F52DC, 0x806F7220,
    0x806F72CC, 0x806F72E8, 0x806F7308, 0x806F7324,
};

// Project+'s "Online Handicap Disable" (its codeset writes a nop here).
constexpr u32 kPPlusHandicapNop = 0x806F3DA4;

// Already Orca's (ORCA.md "Online menu"): sqMenuMain::setNext's old online entries, now the block
// that starts sqVsMelee and the back out's helper. (The exit code Orca keeps at sqVsMelee+0x18 is
// in that object's heap slack, which leaves 4 bytes: no place for the match block.)
constexpr Range kOnlineMenuBlock{0x806DC8C8, 0x806DC920};

namespace Detail
{
constexpr bool Free(const Range& range)
{
  if (range.begin % 4 != 0 || range.end % 4 != 0 || range.end <= range.begin ||
      !kSoraSceneText.Contains(range.begin) || !kSoraSceneText.Contains(range.end - 1) ||
      range.Overlaps(kOnlineMenuBlock) || range.Contains(kPPlusHandicapNop))
  {
    return false;
  }
  for (const Range& create : kSequenceCreates)
  {
    if (range.Overlaps(create))
      return false;
  }
  for (const u32 word : kSoraMeleeRelocations)
  {
    if (range.Contains(word))
      return false;
  }
  return true;
}

constexpr bool CavesValid()
{
  for (std::size_t i = 0; i < kCodeCaves.size(); ++i)
  {
    if (!Free(kCodeCaves[i]) || kCodeCaves[i].Overlaps(kMatchBlock))
      return false;
    if (i > 0 && kCodeCaves[i - 1].end > kCodeCaves[i].begin)
      return false;
  }
  return true;
}
}  // namespace Detail

static_assert(kMatchBlock.Size() >= 96 && kMatchBlock.begin % 32 == 0 &&
              kMatchBlock.end % 32 == 0 && kMatchBlock.end <= kMatchBlockLimit);
static_assert(Detail::Free(kMatchBlock) && Detail::Free({kMatchBlock.begin, kMatchBlockLimit}));
static_assert(Detail::CavesValid());
}  // namespace Orca::UX::FreeSpace
