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
// itself shows it over the fighter and on the results, as a tag the player picked would be; and
// each player's own controls in that tag, so they play with their own layout in a friend's game.
//
// Brawl rev 2 (RSBE01; Project+ runs the same executable), found under the harness:
//   - the save's tags: g_GameGlobal (0x805A00E0) +0x28 -> records (0x90172D40 in both games),
//     tag i at +0xE0 + i * 0x124, its name first: up to 5 UTF-16BE characters and a 0; a tag with
//     an empty name is unused;
//   - a tag's controls (the game's own tag code, which copies the defaults at 0x8004D26C, and
//     Project+'s CSSCustomControls.asm agree): +0x0C rumble (1 on), +0x14 the layout, 0x2D bytes,
//     made from the defaults at 0x80406938: GameCube 12 (L, R, Z, D-pad up, side, down, A, B,
//     C-stick, Y, X, then tap jump in bit 0x80), Wii Remote 8, with Nunchuk 12 (its last: tap jump
//     0x40, shake smash 0x80), Classic 13 (its last: tap jump 0x80). Each other byte is an action
//     (0 attack, 1 special, 2 jump, 3 shield, 4 grab, 5 smash, 9 taunt, 0xA-0xC the taunts,
//     0xE none);
//   - as a match starts (scMelee's first frame) the game copies the layout of each port's tag into
//     its per-port table (0x805B7480, pointed to by 0x805A00C8: the four GameCube layouts, then
//     each port's other three), which the match's input reads: what a port's tag holds when the
//     match starts is what that port plays with (a harness run that turned a tag's A into jump and
//     its tap jump off on the character select changed only that port's row, at scMelee frame 1,
//     and the tap-jump bit of every input the match buffered for it; Project+ alike);
//   - the character select: scene manager (0x805A0060) +4 -> the scene ("scSelctCharacter"),
//     +0x400 -> muSelCharTask, +0x44 + 4 * i -> player area i (GameCube port i), whose +0x1B0 is
//     i again, +0x1B4 the player kind (1: a human has joined), +0x1C8 the tag (-1: none);
//   - joining a port clears its tag to -1, and leaving the screen copies each area's tag into the
//     match's player data (gmPlayerInitData +0x0C name, +0x18 tag index), which the fighter's
//     name above it and the results read.
// So at every frame boundary on the character select, a joined port of a named player with no tag
// gets one: the tag already holding that name, else the highest unused tag, made as the game makes
// one (name, rumble on, the game's default controls from 0x80406938); a name another port already
// wears gets its port number as its last character instead (SAND2). A port that carries its
// player's controls (PortInfo::controls) has them written into that tag as it gets it, so the
// port plays the match with them; a port with controls and no name the game can show gets the tag
// "P<port>", which looks like the game's own label. What it writes depends only on emulated memory
// and the session's synced port list, the same on both machines at the same frame, and it is
// idempotent (a re-run frame writes the same bytes, a port that has a tag is left alone, the
// player's own choice included; a player who sets "no tag" gets theirs back the next frame). A
// port with neither a name nor controls gets nothing.
//
// The names and controls are the host's (Rollback/OnlineMatch.cpp, "Ports"): each port's values
// apply from a frame the host picks, and every machine has them before it runs that frame (a
// joiner gets them with the keyframe offer; a friend already playing hears them before the host
// plugs the newcomer in, which waits for every player's acknowledgement: Session::RequireValues).
// So two machines never write different bytes for a port at the same frame. (Before that channel, a
// name known at a frame on one machine and not the other desynced: ORCA_UX_TEST_LATE_NAME still
// shows what that did.) Not yet: the character select's name plate keeps "PLAYER n" until the game
// redraws it (its own setter, 0x8069B1CC, is called only when the player picks a tag); that needs a
// game-side code.
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

// The CPU thread's emulated memory (at a frame boundary, through the guard).
class GuardMemory final : public GuestMemory
{
public:
  explicit GuardMemory(const Core::CPUThreadGuard& guard) : m_guard(guard) {}
  bool Valid(u32 address) const override;
  u32 Read32(u32 address) const override;
  u16 Read16(u32 address) const override;
  u8 Read8(u32 address) const override;
  void Write8(u32 address, u8 value) override;
  void Write16(u32 address, u16 value) override;
  void Write32(u32 address, u32 value) override;

private:
  const Core::CPUThreadGuard& m_guard;
};

// A player's controls as the session carries them (PortInfo::controls): a tag's rumble byte, then
// its layout (above), CONTROLS_PROFILE_SIZE bytes. Anything else is no profile.
constexpr u32 CONTROLS_LAYOUT_SIZE = 0x2D;
constexpr u32 CONTROLS_PROFILE_SIZE = 1 + CONTROLS_LAYOUT_SIZE;

// One frame boundary's writes for these ports (the plug-in list in effect). Returns how many ports
// were given a tag. A profile's bytes reach the tag only as the game's own menus could have set
// them: rumble 0 or 1, the tap-jump and shake-smash bytes only their bits, any other byte an action
// up to 0xE, else the game's default for that byte (a profile from another player's machine is
// never trusted further; the result is the same on every machine).
int ApplyNameTags(GuestMemory& memory, const std::vector<Events::PortInfo>& ports);

// At a frame boundary (Events' frame callback: every boundary, first runs and re-runs, before the
// session saves anything there, so snapshots and keyframes hold what it wrote), with the ports in
// effect at that frame as the session computes them from its synced seat plan.
void WriteNameTags(const Core::CPUThreadGuard& guard, const std::vector<Events::PortInfo>& ports);

// This machine's player's own controls in its own game (reads only), for the session to carry
// into a game the player joins or hosts: the tag `port`'s player wears on the character select;
// else the tag it last wore there (`last_worn`, kept by the caller across boundaries: the name of
// that tag), if the save still has it; else the tag holding `own_name` as a Brawl tag. Empty when
// none applies (the player plays with the game's defaults, which is what the other machine's game
// gives a port with no profile anyway).
std::vector<u8> ReadOwnControls(const GuestMemory& memory, int port, std::string_view own_name,
                                std::u16string* last_worn);

// The frame hook's part: at a first run while this game plays alone (Events' `alone`), its own
// port's controls as ReadOwnControls finds them, published for the session
// (Events::SetOwnControls). Only Brawl rev 2 and Project+.
void ReadOwnControlsFrame(const Core::CPUThreadGuard& guard,
                          const std::vector<Events::PortInfo>& ports);
}  // namespace Orca::UX
