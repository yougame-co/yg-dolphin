// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/FreeSpace.h"

// The match block (ORCA.md "Ranked sets"): Orca's own data in the game's memory for a queue match,
// at FreeSpace::kMatchBlock, so every rollback snapshot and drop-in keyframe carries it and both
// machines compute the same thing from it. Written only by Orca's frame hook, as a pure function of
// memory (and the synced port list), on first runs and re-runs alike; nothing else reads or writes
// it. Before Orca writes it the block holds the game's dead code (FreeSpace.h), so a reader checks
// the magic first.
//
// Layout (offsets from the block's start, big-endian like the game):
//   +0x00 .. +0x1F  the header: magic "YGM1", version, mode, ruleset (the queue room's, written at an
//                   alone boundary; see the Ranked sets section for the lifecycle)
//   +0x20 .. +0xBF  the ranked set's state (RankedSet.h): the score, each game's record, the fight
//                   being followed
//   +0xC0 .. +0x1FF the stage and character steps (the stage-select tracks), free here
namespace Orca::UX
{
class GuestMemory;

namespace MatchBlock
{
constexpr u32 BASE = FreeSpace::kMatchBlock.begin;
constexpr u32 SIZE = FreeSpace::kMatchBlock.Size();

constexpr u32 MAGIC = 0x59474D31;  // "YGM1"
constexpr u8 VERSION = 1;

// Header.
constexpr u32 HDR_MAGIC = 0x00;    // u32
constexpr u32 HDR_VERSION = 0x04;  // u8
constexpr u32 HDR_MODE = 0x05;     // u8 Mode
constexpr u32 HDR_RULESET = 0x06;  // u8 Ruleset
constexpr u32 HDR_END = 0x20;

enum class Mode : u8
{
  None = 0,
  Casual = 1,
  Ranked = 2,
};

enum class Ruleset : u8
{
  None = 0,
  Brawl = 1,  // Supernova 2025
  PPlus = 2,  // Project+ 2024 (Proposed stage list, Recommended procedure)
};

struct Header
{
  Mode mode = Mode::None;
  Ruleset ruleset = Ruleset::None;
  bool operator==(const Header&) const = default;
};

// The set's state.
constexpr u32 SET_BEGIN = 0x20;
constexpr u32 SET_GAMES = 0x20;        // u8: games recorded (wins and ties; void games aren't)
constexpr u32 SET_WINS = 0x21;         // u8[2]: games won by ports 1 and 2
constexpr u32 SET_DONE = 0x23;         // u8: 0 open, 1 won (SET_WINNER), 2 void
constexpr u32 SET_WINNER = 0x24;       // u8: the set's winning port (0-based), 0xFF none
constexpr u32 SET_LAST_WINNER = 0x25;  // u8: the last decided game's winning port, 0xFF none
constexpr u32 SET_TIEBREAK = 0x26;     // u8: 1 the next game is a tiebreak game
constexpr u32 SET_TB_STREAK = 0x27;    // u8: tied games in a row
constexpr u32 SET_FIGHT = 0x28;        // u8: 1 while a fight is followed (start to results)
constexpr u32 SET_FIGHT_FLAGS = 0x29;  // u8: FIGHT_* below
constexpr u32 SET_LEDGE = 0x2A;        // u8[2]: ledge grabs this fight (255 at most)
constexpr u32 SET_FIGHT_START = 0x2C;  // u32: the fight's first frame (the game's report id)
constexpr u32 SET_SNAP_STOCKS = 0x30;  // u8[2]: stocks when time ran out
constexpr u32 SET_SNAP_PERCENT = 0x32;  // u16[2]: percent when time ran out
constexpr u32 SET_ON_LEDGE = 0x36;      // u8[2]: on the ledge at the last frame (for counting)
constexpr u32 SET_RECORDS = 0x40;       // GameRecord x MAX_RECORDS
constexpr u32 RECORD_SIZE = 0x10;
constexpr int MAX_RECORDS = 8;
constexpr u32 SET_END = SET_RECORDS + RECORD_SIZE * MAX_RECORDS;  // 0xC0

constexpr u8 FIGHT_BETWEEN = 0x01;  // left the fight scene (between scenes)
constexpr u8 FIGHT_SUDDEN = 0x02;   // came back into a fight: sudden death
constexpr u8 FIGHT_TIMEUP = 0x04;   // time ran out (the snapshot holds stocks and percent)

constexpr u8 NO_PORT = 0xFF;

// How a game was decided.
enum class How : u8
{
  None = 0,
  Results = 1,  // the results screen's winner (a KO, or the game's own time-out rules)
  Stocks = 2,   // time-out: more stocks
  Percent = 3,  // time-out: lower percent
  Ledge = 4,    // time-out: the other player went over the ledge-grab limit
  Tie = 5,      // no winner: a tiebreak game follows (Brawl), or none (Project+)
};

// One game: +0x00 u32 its fight's first frame, +0x04 u8 winner port (NO_PORT a tie), +0x05 u8 How,
// +0x06 u8 the stage, +0x07 u8 flags (bit 0: a tiebreak game, bit 1: time ran out), +0x08 u8[2]
// characters, +0x0A u8[2] stocks left (0xFF timed), +0x0C u8[2] ledge grabs, +0x0E u16 reserved.
struct GameRecord
{
  u32 start = 0;
  u8 winner = NO_PORT;
  How how = How::None;
  u8 stage = 0xFF;
  bool tiebreak = false;
  bool timeout = false;
  std::array<u8, 2> characters{0xFF, 0xFF};
  std::array<u8, 2> stocks{0xFF, 0xFF};
  std::array<u8, 2> ledge{0, 0};
  bool operator==(const GameRecord&) const = default;
};

struct SetState
{
  u8 games = 0;
  std::array<u8, 2> wins{0, 0};
  u8 done = 0;
  u8 winner = NO_PORT;
  u8 last_winner = NO_PORT;
  bool tiebreak = false;
  u8 tb_streak = 0;
  bool fight = false;
  u8 fight_flags = 0;
  std::array<u8, 2> ledge{0, 0};
  u32 fight_start = 0;
  std::array<u8, 2> snap_stocks{0, 0};
  std::array<u16, 2> snap_percent{0, 0};
  std::array<u8, 2> on_ledge{0, 0};
  std::array<GameRecord, MAX_RECORDS> records{};
  bool operator==(const SetState&) const = default;

  // The game being played (or about to be): one more than the games decided.
  int GameNumber() const { return wins[0] + wins[1] + 1; }
  // The stages a port won a game on in this set, as a mask of stage ids below 64 (for the stage
  // select's Dave's Stupid Rule).
  u64 StagesWonBy(int port) const;
};

// The header, if the block holds one (magic and version).
std::optional<Header> ReadHeader(const GuestMemory& memory);
// Writes the header (only the bytes that differ). A header with another mode or ruleset than the
// one there, or none there before, starts a fresh set state. Returns how many bytes changed.
int WriteHeader(GuestMemory& memory, const Header& header);
// Clears the header and the set state (the block reads as none). Returns how many bytes changed.
int Clear(GuestMemory& memory);

SetState ReadSet(const GuestMemory& memory);
// Writes the bytes of `state` that differ from the block's. Returns how many bytes changed.
int WriteSet(GuestMemory& memory, const SetState& state);

// Whether the block is in mapped memory (the game is booted).
bool Mapped(const GuestMemory& memory);
}  // namespace MatchBlock
}  // namespace Orca::UX
