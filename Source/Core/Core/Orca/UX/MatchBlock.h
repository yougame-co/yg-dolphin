// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/FreeSpace.h"

// The match block (ORCA.md "Online rules"): what a queue match's rules keep in the game's own
// memory, at FreeSpace::kMatchBlock (dead code of sora_scene's online sequences, the same address
// in Brawl rev 2 and Project+). Every rollback snapshot and drop-in keyframe carries it, like the
// rest of MEM1. Big-endian, like the game.
//
// Before Orca writes it the block holds the game's dead code (its first word is
// FreeSpace::kMatchBlockOriginalWord), so a reader checks the magic and treats anything else as no
// block: friends rooms and solo play never have one.
//
// Who writes what:
// - The header (and the saved rules), only at a boundary where this machine's game is its player's
//   alone (Events.h `alone`): the header its room calls for, a queue room's mode and ruleset, or
//   none. Never with a peer plugged in or on the way.
// - Everything else, game logic: the frame hook at every boundary, first runs and re-runs, as a
//   pure function of memory (OnlineRules.h).
namespace Orca::UX::MatchBlock
{
constexpr u32 BASE = FreeSpace::kMatchBlock.begin;  // 0x806F2380
constexpr u32 SIZE = FreeSpace::kMatchBlock.Size();  // 512

// ---- Header, +0x00 ----
constexpr u32 MAGIC = BASE + 0x00;  // u32 'YGM1' while the block is Orca's
constexpr u32 MAGIC_VALUE = 0x59474D31;
constexpr u32 VERSION = BASE + 0x04;  // u8
constexpr u8 VERSION_VALUE = 1;
constexpr u32 MODE = BASE + 0x05;     // u8 Mode
constexpr u32 RULESET = BASE + 0x06;  // u8 Ruleset
// +0x07..+0x0F: reserved (zero).
constexpr u32 HEADER_SIZE = 0x10;

// What the header's mode byte says. A toggled patch group reads it as the word at VERSION
// (`when 806F2384 & 00FF0000 != 00000000`: mode not none) together with the magic.
enum class Mode : u8
{
  None = 0,
  Casual = 1,
  Ranked = 2,
};

enum class Ruleset : u8
{
  None = 0,
  Brawl = 1,  // Supernova 2025: 3 stocks, 8:00, BF PS1 LC SV YI + FD DP
  PPlus = 2,  // Project+ 2024 Recommended procedure, the 2024 Proposed stage list
};

// ---- Set state, +0x10..+0x7F: the ranked set (Phase 2 and 3; zero until then) ----
constexpr u32 SET = BASE + 0x10;
constexpr u32 SET_SIZE = 0x70;

// ---- Step state, +0x80..+0xBF: strikes, bans and picks (Phase 2; zero until then) ----
constexpr u32 STEP = BASE + 0x80;
constexpr u32 STEP_SIZE = 0x40;

// ---- Phase 1's own, +0xC0..+0xCF ----
// u16: frames on the ranked character select since the ready timer started (it counts while one
// port has a character and another doesn't); 0 while it doesn't run.
constexpr u32 CSS_TIMER = BASE + 0xC0;
// u8: the port (1-4) that didn't ready in time; 0 none. Set once when the timer runs out, cleared
// when the character select is left.
constexpr u32 CSS_NO_SHOW = BASE + 0xC2;
// u8: bit 0, a fight began since the header was written (the no-show applies before it only).
constexpr u32 FOUGHT = BASE + 0xC3;
// u8: the ports in play (bit n: port n+1), from the session's synced plan at each boundary.
constexpr u32 PLUGGED = BASE + 0xC4;
constexpr u32 PHASE1_SIZE = 0x10;

// ---- Saved at the header's alone write, given back when it is cleared ----
// The player's own rules before the lock (gmSetRule +0x02..+0x0B, 10 bytes) and the record's menu
// data words the lock writes (items and the Random Stage Switch).
constexpr u32 SAVED_RULES = BASE + 0xD0;       // 10 bytes, gmSetRule +0x02..+0x0B
constexpr u32 SAVED_ITEMS = BASE + 0xE0;       // 3 words: menu data +0x00, +0x08, +0x0C
constexpr u32 SAVED_STAGES = BASE + 0xEC;      // 2 words: menu data +0x20, +0x24
// Project+ only: the first RSS_BYTES bytes of its stage switch data (RSS_EXDATA) before the lock.
constexpr u32 SAVED_RSS = BASE + 0x100;
constexpr u32 SAVED_RSS_SIZE = 0xA8;
constexpr u32 SAVED_FLAGS = BASE + 0x1F0;  // u8: bit 0 rules saved, bit 1 RSS saved

static_assert(SAVED_RSS + SAVED_RSS_SIZE <= SAVED_FLAGS);
static_assert(SAVED_FLAGS < BASE + SIZE);
}  // namespace Orca::UX::MatchBlock
