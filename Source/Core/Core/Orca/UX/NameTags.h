// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"

namespace Core
{
class CPUThreadGuard;
}

// Each player's YouGame username in Brawl's own name tags (ORCA_ONLINE_UX.md 1C), so the game
// itself shows it over the fighter and on the results, as a tag the player picked would be.
//
// Brawl rev 2 (RSBE01; Project+ runs the same executable), found under the harness:
//   - the save's tags: g_GameGlobal (0x805A00E0) +0x28 -> records, tag i at +0xE0 + i * 0x124,
//     its name first: up to 5 UTF-16BE characters and a 0; a tag with an empty name is unused;
//   - the character select: scene manager (0x805A0060) +4 -> the scene ("scSelctCharacter"),
//     +0x400 -> muSelCharTask, +0x44 + 4 * i -> player area i (GameCube port i), whose +0x1B0 is
//     i again, +0x1B4 the player kind (1: a human has joined), +0x1C8 the tag (-1: none);
//   - joining a port clears its tag to -1, and leaving the screen copies each area's tag into the
//     match's player data (gmPlayerInitData +0x0C name, +0x18 tag index), which the fighter's
//     name above it and the results read.
// So at every frame boundary on the character select, a joined port of a named player with no tag
// gets one: the tag already holding that name, else the highest unused tag, made as the game makes
// one (name, rumble on, the game's default controls from 0x80406938); a name another port already
// wears gets its port number as its last character instead (SAND2). What it
// writes depends only on emulated memory and the session's synced port list, the same on both
// machines at the same frame, and it is idempotent (a re-run frame writes the same bytes, a port
// that has a tag is left alone, the player's own choice included; a player who sets "no tag" gets
// theirs back the next frame). A port whose name is empty (not
// known yet on this machine) gets nothing, and a name gives the same bytes whenever it became known.
// Known gap until the session sends host-authoritative names with the plug-in: if one machine has
// a name at frame F and the other still has "", their writes differ at F (a desync). Not yet: the character select's
// name plate keeps "PLAYER n" until the game redraws it (its own setter, 0x8069B1CC, is called only
// when the player picks a tag); that needs a game-side code.
namespace Orca::UX
{
constexpr int TAG_CHARS = 5;
constexpr int TAG_SLOTS = 120;

// A YouGame username as a Brawl tag: ASCII letters uppercased, digits kept, '-', '.', '!' and '?'
// kept, '_' and spaces as '-', anything else dropped; at most 5 characters. Pure and the same on
// every machine (no locale). Empty when nothing usable is left. Any bytes are fine (the session's
// names are printable UTF-8 up to 64 bytes). Nothing here reads PortInfo.remote, which differs per
// machine: only the port number and the name decide what is written.
std::u16string BrawlTag(std::string_view username);

// Emulated memory as the writer needs it (the tests give it a map).
class GuestMemory
{
public:
  virtual ~GuestMemory() = default;
  virtual bool Valid(u32 address) const = 0;  // a mapped, readable RAM address
  virtual u32 Read32(u32 address) const = 0;
  virtual u16 Read16(u32 address) const = 0;
  virtual u8 Read8(u32 address) const = 0;
  virtual void Write8(u32 address, u8 value) = 0;
  virtual void Write16(u32 address, u16 value) = 0;
  virtual void Write32(u32 address, u32 value) = 0;
};

// One frame boundary's writes for these ports (the plug-in list in effect). Returns how many ports
// were given a tag.
int ApplyNameTags(GuestMemory& memory, const std::vector<Events::PortInfo>& ports);

// At a frame boundary (Events' frame callback: every boundary, first runs and re-runs, before the
// session saves anything there, so snapshots and keyframes hold what it wrote), with the ports in
// effect at that frame as the session computes them from its synced seat plan.
void WriteNameTags(const Core::CPUThreadGuard& guard, const std::vector<Events::PortInfo>& ports);
}  // namespace Orca::UX
