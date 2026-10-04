// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/FreeSpace.h"

// The match block (ORCA.md "Online rules", "Ranked stage flow", "Ranked sets"): what a queue
// match's rules keep in the game's own memory, at FreeSpace::kMatchBlock (dead code of sora_scene's
// online sequences, the same address in Brawl rev 2 and Project+). Every rollback snapshot and
// drop-in keyframe carries it, like the rest of MEM1. Big-endian, like the game.
//
// Before Orca writes it the block holds the game's dead code (its first word is
// FreeSpace::kMatchBlockOriginalWord), so a reader checks the magic and treats anything else as no
// block: friends rooms and solo play never have one.
//
// Who writes what:
// - The header (and the saved rules), only at a boundary where this machine's game is its player's
//   alone (Events.h `alone`): the header its room calls for, a queue room's mode, ruleset, coin and
//   room, or none (OnlineRules.h WriteHeader, the only header writer). Never with a peer plugged in
//   or on the way. A new header (from none, or another mode, ruleset or room) starts every
//   track's state fresh (FreshSet).
// - Everything else, game logic: the frame hook at every boundary, first runs and re-runs, as a
//   pure function of memory (OnlineRules.h, RankedPPlus.h, BrawlStages.h, RankedSet.h,
//   CharOrder.h).
//
// Layout (offsets from BASE):
//   +0x00..+0x0F  header: magic, version, mode, ruleset, coin, the room's hash, the queue flags
//   +0x10..+0x7F  Project+'s stage flow's set (State below; a casual game's preferred stages at
//                 +0x40, its fight count at +0x10)
//   +0x80..+0xBF  the stage flows' steps (State below for Project+; BrawlStages.h for Brawl, whose
//                 casual pick keeps its preferred stages at +0xA3 and its coin at +0xA8)
//   (The casual stage pick needs no range of its own: casual and ranked never share a block, and
//   +0x140..+0x17F, once offered for it, is Project+'s SAVED_RSS.)
//   +0xC0..+0xCF  Phase 1's own (the ready timer)
//   +0xD0..+0xF3  the player's own rules, saved at the header's write
//   +0xF4..+0xFF  the stage select's two cursors, shared (StageCursors.h)
//   +0x100..+0x1A7 Project+'s stage switch data, saved at the header's write
//   +0x1B4..+0x1BF the stage select's two cursors (StageCursors.h)
//   +0x1C0..+0x1EF the queue's character select (Queue.h: ready flags, the ready timer, the raw
//                 buttons, the opponent's pick)
//   +0x1F0        the saved rules' flags
//   +0x200..+0x20F the character order (CharOrder.h)
//   +0x210..+0x28F the ranked set as the results track follows it (SetBlock.h, authoritative)
//   +0x290..+0x297 a friend's drop-in from the menus (OnlineMenu.h FriendsMove)
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::MatchBlock
{
constexpr u32 BASE = FreeSpace::kMatchBlock.begin;  // 0x806F2380
constexpr u32 SIZE = FreeSpace::kMatchBlock.Size();  // 512
// The block with its free tail (the character order and the set), up to the next relocated word.
constexpr u32 FULL_SIZE = FreeSpace::kMatchBlockLimit - FreeSpace::kMatchBlock.begin;

// ---- Header, +0x00 ----
constexpr u32 MAGIC = BASE + 0x00;  // u32 'YGM1' while the block is Orca's
constexpr u32 MAGIC_VALUE = 0x59474D31;
constexpr u32 VERSION = BASE + 0x04;  // u8
constexpr u8 VERSION_VALUE = 1;
constexpr u32 MODE = BASE + 0x05;     // u8 Mode
constexpr u32 RULESET = BASE + 0x06;  // u8 Ruleset
// u8: game 1's first striker, port 0 or 1: one bit of the room code's hash (CoinFromCode). The
// server makes the code; neither player chooses it.
constexpr u32 COIN = BASE + 0x07;
// u32: the room code's hash (RoomHash): a new queue room with the same mode and ruleset still
// starts a fresh set.
constexpr u32 ROOM = BASE + 0x08;
// u8: the queue's flags (Queue.h). FLAG_SOLO: this game is its player's own, on the queue's
// character select before a match (no room of the queue's; the "With Anyone" pick with an app that
// has `queue2`). FLAG_QUEUE2: the Slippi-style character select (ready with Start, the 30 s ready
// timer, the opponent's pick written in): the host's app has `queue2`. A toggled patch group reads
// the word at FLAGS (`when 806F238C & 01000000 == 00000000`: not solo).
constexpr u32 FLAGS = BASE + 0x0C;
constexpr u8 FLAG_SOLO = 0x01;
constexpr u8 FLAG_QUEUE2 = 0x02;
// +0x0D..+0x0F: reserved (zero).
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

// ---- Set state, +0x10..+0x7F: Project+'s stage flow's set (State below) ----
constexpr u32 SET = BASE + 0x10;
constexpr u32 SET_SIZE = 0x70;

// ---- Step state, +0x80..+0xBF: strikes, bans and picks ----
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
// u32: the frame hook's frame number + 1 when the ready timer started (0: not running), so the
// timer is a function of the frame number, the same however often a boundary runs.
constexpr u32 CSS_TIMER_START = BASE + 0xC8;
constexpr u32 PHASE1_SIZE = 0x10;

// ---- Saved at the header's alone write, given back when it is cleared ----
// The player's own rules before the lock (gmSetRule +0x02..+0x0B, 10 bytes) and the record's menu
// data words the lock writes (items and the Random Stage Switch).
constexpr u32 SAVED_RULES = BASE + 0xD0;   // 10 bytes, gmSetRule +0x02..+0x0B
constexpr u32 SAVED_ITEMS = BASE + 0xE0;   // 3 words: menu data +0x00, +0x08, +0x0C
constexpr u32 SAVED_STAGES = BASE + 0xEC;  // 2 words: menu data +0x20, +0x24
// Project+ only: the first RSS_BYTES bytes of its stage switch data (RSS_EXDATA) before the lock.
constexpr u32 SAVED_RSS = BASE + 0x100;
constexpr u32 SAVED_RSS_SIZE = 0xA8;
constexpr u32 SAVED_FLAGS = BASE + 0x1F0;  // u8: bit 0 rules saved, bit 1 RSS saved

// ---- The stage select's two cursors (StageCursors.h) ----
// The free bytes after the saved stages (+0xF4..+0xFF) and before the queue's region
// (+0x1B4..+0x1BF; +0x1A8..+0x1B3 are the character select's).
constexpr u32 STAGE_CURSORS_SHARED = BASE + 0xF4;
constexpr u32 STAGE_CURSORS_SHARED_SIZE = 0x0C;
constexpr u32 STAGE_CURSORS = BASE + 0x1B4;
constexpr u32 STAGE_CURSORS_SIZE = 0x0C;

// ---- The queue's character select (Queue.h), +0x1C0..+0x1EF ----
// (The sprint assigned this track +0x100..+0x13F, which is Project+'s saved stage switch data,
// SAVED_RSS; +0x1A8..+0x1EF is the free run before SAVED_FLAGS.)
constexpr u32 QUEUE = BASE + 0x1C0;
constexpr u32 QUEUE_SIZE = 0x30;

// ---- Past the 512 bytes, in the same dead code up to kMatchBlockLimit ----
constexpr u32 CHAR_ORDER = BASE + 0x200;  // 16 bytes (CharOrder.h)
constexpr u32 CHAR_ORDER_SIZE = 0x10;
constexpr u32 RANKED_SET = BASE + 0x210;  // SetBlock.h's set (+0x210..+0x28F)

// ---- A friend's drop-in from the menus (OnlineMenu.h FriendsMove), +0x290..+0x297 ----
// The last free bytes before kMatchBlockLimit (+0x180..+0x18F, first planned for this, is Project+'s
// SAVED_RSS). Unlike the rest of the block these are kept in every session, solo and friends rooms
// alike, with or without a header (a queue room's header only stops the move), wherever the block's
// memory is Present.
constexpr u32 FRIENDS = BASE + 0x290;
// u8: FRIENDS_TAG_VALUE once Orca's; anything else is the dead code's byte (nothing kept yet).
constexpr u32 FRIENDS_TAG = FRIENDS + 0;
constexpr u8 FRIENDS_TAG_VALUE = 0xF5;
// u8: the ports plugged in at the last boundary (bit n: port n+1), from the session's synced plan.
constexpr u32 FRIENDS_SEEN = FRIENDS + 1;
// u8: 1 while a friend who plugged in waits for the host's main menu, to go to With Friends'
// character select together.
constexpr u32 FRIENDS_PENDING = FRIENDS + 2;
// u32: the frame hook's frame number + 1 from which the main menu has been running (0: it isn't),
// so how long it has run is a function of the frame number, the same however often a boundary runs.
constexpr u32 FRIENDS_MENU_SINCE = FRIENDS + 4;
constexpr u32 FRIENDS_END = FRIENDS + 8;

static_assert(SAVED_RSS + SAVED_RSS_SIZE <= SAVED_FLAGS);
static_assert(SAVED_STAGES + 8 <= STAGE_CURSORS_SHARED &&
              STAGE_CURSORS_SHARED + STAGE_CURSORS_SHARED_SIZE <= SAVED_RSS);
static_assert(SAVED_RSS + SAVED_RSS_SIZE <= STAGE_CURSORS &&
              STAGE_CURSORS + STAGE_CURSORS_SIZE <= QUEUE);
static_assert(RANKED_SET + 0x80 <= FRIENDS && FRIENDS_END <= FreeSpace::kMatchBlockLimit);
static_assert(FRIENDS_MENU_SINCE % 4 == 0);
static_assert(SAVED_RSS + SAVED_RSS_SIZE <= QUEUE && QUEUE + QUEUE_SIZE <= SAVED_FLAGS);
static_assert(SAVED_FLAGS < BASE + SIZE);
static_assert(CHAR_ORDER + CHAR_ORDER_SIZE <= RANKED_SET);
static_assert(CSS_TIMER_START + 4 <= BASE + 0xC0 + PHASE1_SIZE);

// ---- Project+'s stage flow's state (RankedPPlus.h), +0x00..+0x8F ----
//   header   +0x00 u32 magic "YGM1"   +0x04 u8 version   +0x05 u8 mode   +0x06 u8 ruleset
//            +0x07 u8 coin
//   set      +0x10 u8 games counted   +0x11 u8 port 1's wins   +0x12 u8 port 2's wins
//            +0x13 u8 the last counted game's winner (0xFF none)
//            +0x14 u16 stages port 1 won on   +0x16 u16 stages port 2 won on (ruleset indices)
//            +0x18 u8 the set is decided   +0x19 u8 its winner (0xFF none)
//            +0x1A u8 a counted fight is on (entered from the stage flow)
//            +0x1B u8 its stage (ruleset index, 0xFF unknown)
//            +0x1C u8 port 1's character, +0x1D port 2's (gmCharacterKind, 0xFF unknown)
//            +0x20 4 bytes per counted game (up to 8): winner, stage, port 1's and 2's characters
//            (casual: +0x10 counts the fights begun from the stage select instead, and nothing
//            else of the set is used)
//            +0x40 u8 port 1's preferred stage, +0x41 port 2's (casual; ruleset index, 0xFF none)
//   steps    +0x80 u8 the stage flow: 0 idle, 1 running on the stage select
//            +0x81 u8 step   +0x82 u8 the port whose turn it is (0xFF none)
//            +0x83 u8 strikes or bans made in this step   +0x84 u16 struck or banned this game
//            +0x88 u32 the step's deadline (the frame hook's frame number)
//            +0x8C u8 the pick's timer ran out: the default pick is being made   +0x8D u8 its stage
//            +0x8E u8 the default pick's A is pressed this frame
//            +0x8F u8 why the flow is picking (AUTO_*): its timer, the player's pick, the two
//            players' agreement, a casual game's result
constexpr u32 STATE_SIZE = 0x90;
static_assert(BASE + STATE_SIZE <= FreeSpace::kMatchBlock.end);

constexpr u8 MODE_NONE = 0;
constexpr u8 MODE_CASUAL = 1;
constexpr u8 MODE_RANKED = 2;

constexpr u8 NONE = 0xFF;
// Why the stage flow is picking a stage (State::auto_why).
constexpr u8 AUTO_TIMER = 0;   // the pick's timer ran out: its default
constexpr u8 AUTO_PICKED = 1;  // the picker's A on a stage
constexpr u8 AUTO_AGREED = 2;  // both players proposed the same stage
constexpr u8 AUTO_CASUAL = 3;  // a casual game's preferred stages (the same, or the coin's)
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
  u8 auto_why = 0;          // AUTO_TIMER, AUTO_PICKED, AUTO_AGREED or AUTO_CASUAL
  // A casual game's preferred stages (Project+'s casual stage pick, RankedPPlus.h).
  std::array<u8, 2> prefs{NONE, NONE};
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
// Writes `state`'s bytes (+0x00..+0x8F; the header's room hash and the queue's flags are not
// State's), only the bytes that differ, and only where Present. Returns how many bytes changed.
int Write(GuestMemory& memory, const State& state);
// No header: the dead code's first word back in place of the magic (nothing else is read without
// it). Returns bytes changed.
int Clear(GuestMemory& memory);

// Game 1's first striker from the room's code: one bit of its FNV-1a hash. The server makes the
// code; neither player chooses it, and both games read the result from the block.
u8 CoinFromCode(std::string_view code);
// The room code's FNV-1a hash, for the header's ROOM.
u32 RoomHash(std::string_view code);
}  // namespace Orca::UX::MatchBlock
