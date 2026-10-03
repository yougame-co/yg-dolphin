// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/FreeSpace.h"

// The match block (ORCA.md "Ranked stage flow"): a queue match's mode, ruleset and coin, and a
// ranked set's state, in the game's own memory (FreeSpace.h kMatchBlock, the same address in Brawl
// and Project+). Rollback snapshots and drop-in keyframes carry it like the rest of MEM1, so every
// machine has the same block at the same frame; only the frame hook writes it, as a pure,
// idempotent function of memory and the session's synced data (Events.h).
//
// Who writes the header: the host of a matchmade room, at the boundary after it takes the room,
// while its game is alone (before any keyframe a joiner loads), and the clear at an alone boundary
// once the game no longer hosts or plays in a matchmade room, so a friend who drops in later never
// meets a ranked block (RankedPPlus.h ApplyHeader). Everything after the header is game logic.
//
// Layout (big-endian, offsets from kMatchBlock.begin):
//   header   +0x00 u32 magic "YGM1"   +0x04 u8 version   +0x05 u8 mode   +0x06 u8 ruleset
//            +0x07 u8 coin: game 1's first striker, port 0 or 1 (a hash of the room's code)
//   set      +0x10 u8 games counted   +0x11 u8 port 1's wins   +0x12 u8 port 2's wins
//            +0x13 u8 the last counted game's winner (0xFF none)
//            +0x14 u16 stages port 1 won on   +0x16 u16 stages port 2 won on (ruleset indices)
//            +0x18 u8 the set is decided   +0x19 u8 its winner (0xFF none)
//            +0x1A u8 a counted fight is on (entered from the stage flow)
//            +0x1B u8 its stage (ruleset index, 0xFF unknown)
//            +0x1C u8 port 1's character, +0x1D port 2's (gmCharacterKind, 0xFF unknown)
//            +0x20 4 bytes per counted game (up to 8): winner, stage, port 1's and 2's characters
//   steps    +0x40 u8 the stage flow: 0 idle, 1 running on the stage select
//            +0x41 u8 step   +0x42 u8 the port whose turn it is (0xFF none)
//            +0x43 u8 strikes or bans made in this step   +0x44 u16 struck or banned this game
//            +0x48 u32 the step's deadline (the frame hook's frame number)
//            +0x4C u8 the pick's timer ran out: the default pick is being made   +0x4D u8 its stage
//            +0x4E u8 the default pick's A is pressed this frame
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::MatchBlock
{
constexpr u32 BASE = FreeSpace::kMatchBlock.begin;
constexpr u32 SIZE = 0x60;
static_assert(BASE + SIZE <= FreeSpace::kMatchBlock.end);
constexpr u32 MAGIC = 0x59474D31;  // "YGM1"
constexpr u8 VERSION = 1;

constexpr u8 MODE_NONE = 0;
constexpr u8 MODE_CASUAL = 1;
constexpr u8 MODE_RANKED = 2;

constexpr u8 NONE = 0xFF;
constexpr int MAX_GAMES = 8;

struct GameRecord
{
  u8 winner = NONE;
  u8 stage = NONE;
  std::array<u8, 2> characters{NONE, NONE};
  bool operator==(const GameRecord&) const = default;
};

struct State
{
  // Header.
  u8 mode = MODE_NONE;
  u8 ruleset = 0;
  u8 coin = 0;
  // Set.
  u8 games = 0;
  std::array<u8, 2> score{};
  u8 last_winner = NONE;
  std::array<u16, 2> won_on{};
  bool done = false;
  u8 set_winner = NONE;
  bool fight = false;
  u8 stage = NONE;
  std::array<u8, 2> characters{NONE, NONE};
  std::array<GameRecord, MAX_GAMES> records{};
  // Steps.
  u8 flow = 0;
  u8 step = 0;
  u8 active = NONE;
  u8 step_done = 0;
  u16 struck = 0;
  u32 deadline = 0;
  bool auto_pick = false;
  u8 auto_stage = NONE;
  bool auto_press = false;  // the default pick's A, pressed 4 frames then released 4
  bool operator==(const State&) const = default;

  // A new set: the header's fields and nothing else.
  static State Fresh(u8 mode, u8 ruleset, u8 coin);
};

// The block, or nullopt when it holds no header (the game's dead code, or a newer version).
std::optional<State> Read(const GuestMemory& memory);
// Whether the block's memory is there to write: sora_scene's dead code (its first word,
// FreeSpace.h kMatchBlockOriginalWord) or a block already. Before the game loads sora_scene this
// memory is something else's.
bool Present(const GuestMemory& memory);
// Writes `state` with the header, only the bytes that differ, and only where Present. Returns how
// many bytes changed.
int Write(GuestMemory& memory, const State& state);
// No header: the dead code's first word back in place of the magic (nothing else is read without
// it). Returns bytes changed.
int Clear(GuestMemory& memory);

// Game 1's first striker from the room's code: one bit of its FNV-1a hash. The server makes the
// code; neither player chooses it, and both games read the result from the block.
u8 CoinFromCode(std::string_view code);
}  // namespace Orca::UX::MatchBlock
