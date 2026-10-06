// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/Results.h"
#include "Core/Orca/UX/SetBlock.h"

namespace Core
{
class CPUThreadGuard;
}

// A ranked set in the game's own memory (ORCA.md "Ranked sets"): the frame hook follows each fight
// of a queue match whose match block says ranked, judges it by the set's ruleset when its results
// screen comes, and keeps the score in the block (SetBlock::SetState). Every write is a pure
// function of memory and the frame number (the same on both machines), on first runs and re-runs
// alike, and only bytes that differ are written, so a re-run from a snapshot taken at a boundary
// writes nothing new there. The room hears each game from the block once its frames are final
// (Results.h's tracker), and the stage select reads the score, the last winner and the stages won
// from it.
//
// The ruleset's verdict (Judge):
// - Brawl (Supernova 2025): a KO, the results screen's winner. Out of time (the game's own time-up,
//   or the fight's clock seen at 0): the ledge-grab limit first (over 35 grabs loses, Meta Knight
//   over 20; both over, the limit is ignored; a tiebreak game's 11 and 6), then more stocks, then
//   lower percent (each as it was when time ran out). Still level, or a sudden death (a
//   simultaneous KO): a tie, and the next game is a tiebreak game, 1 stock and 3:00 on the same
//   stage with the same characters. Five ties in a row void the set.
// - Project+ (2024): the results screen's winner. Its own codeset already decides time-outs by
//   stocks then percent and gives an exact tie overtime, so there is nothing to add.
// A game without exactly two humans on ports 1 and 2, or with a no contest, isn't recorded.
// The set ends when a port has won 2 games, or void after SetBlock::MAX_RECORDS games.
namespace Orca::UX::RankedSet
{
constexpr int WINS = 2;
constexpr int TIEBREAK_STREAK_LIMIT = 5;
constexpr int LEDGE_LIMIT = 35;
constexpr int LEDGE_LIMIT_MK = 20;
constexpr int LEDGE_LIMIT_TIEBREAK = 11;
constexpr int LEDGE_LIMIT_TIEBREAK_MK = 6;
// The result info's character id (gmCharacterKind, BrawlHeaders gm_lib.h: 0x18 Meta Knight; 0x16 is
// Ganondorf there).
constexpr u8 META_KNIGHT = 0x18;

// The fight as one frame shows it, where the game's own structures can be read (LiveFight below).
struct Live
{
  bool valid = false;
  // The fight's clock ran out (a timed stock match's remaining time is 0).
  bool time_up = false;
  std::array<u8, 2> stocks{0, 0};
  std::array<u16, 2> percent{0, 0};
  std::array<bool, 2> on_ledge{false, false};
  bool operator==(const Live&) const = default;
};

// What the hook needs from one frame boundary.
struct Facts
{
  Reading::Scene scene = Reading::Scene::Other;
  // On the results screen: the result info.
  ResultBlock block;
  // The result info's decision (gmResultInfo +0x1378): 1 time up.
  u8 decision = 0;
  Live live;
  // The fight's live state was read (Brawl's verdict needs it): an invalid one means the fighters
  // are gone (between two fights in the same scene, or at its end).
  bool live_read = false;
};

// The verdict on one finished fight, or nothing (a void game: not recorded).
std::optional<SetBlock::GameRecord> Judge(SetBlock::Ruleset ruleset, const SetBlock::SetState& set,
                                          const ResultBlock& block, u8 decision);

// One frame boundary: the set as it is after it. `frame` is the frame hook's.
SetBlock::SetState Step(SetBlock::SetState set, SetBlock::Ruleset ruleset, const Facts& facts,
                        u32 frame);

// The rules a Brawl ranked game is played with (written on the menus, never in a fight): 3 stocks
// and 8:00, or 1 stock and 3:00 for a tiebreak game. Returns how many bytes changed.
int WriteSetRules(GuestMemory& memory, const SetBlock::SetState& set, u8 stocks = 3,
                  u8 minutes = 8);

// The fight's live state, where it can be read (reads only; invalid where it can't).
Live ReadLiveFight(const GuestMemory& memory);

// Facts from memory (reads only); the fight's live state only with `live`.
Facts ReadFacts(const GuestMemory& memory, bool live);

// The frame hook's part (first runs and re-runs alike; `resimulating` only quiets the log): follows
// the set in a ranked block.
void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating);

// The set score line for the overlay (local UI only), the local player's score first:
// "Tiebreak game · 1–1", "Set over · 2–1", "Set void · 1–0"; on the results screen of an ordinary
// game, the game just played and the score ("Game 1 · 1–0": that screen has no dots; Luis,
// 2026-10-05: "it should correctly say game 1 and 2 respectively because we are on the victory
// screens"); empty elsewhere for an ordinary game (the dots say which game it is) and outside a
// ranked set.
std::string ScoreLine(const SetBlock::SetState& set, int local_port, bool results = false);
// A matchmade game's result toast (local UI only), the local player's score first: "You won ·
// 1–0", "You lost · 0–1", "Draw · played again", "That game didn't count". `out` is the room's
// verdict for this player: "won", "lost", "draw", anything else a void.
std::string ResultToast(std::string_view out, int mine, int theirs);
// The last line the hook published (any thread), for this machine's player (SetLocalPort); empty
// during a fight.
std::string CurrentScoreLine();
void SetLocalPort(int port);

// The set's games for the overlay's dots (local UI only): games[i] the port (0 or 1) that won the
// set's i-th decided game, -1 for one still to play (a tied game isn't one); `current` the game
// being played (0-based), -1 once the set is over. `on` in a ranked set on the menus only.
struct Dots
{
  bool on = false;
  std::array<int, 3> games{-1, -1, -1};
  int current = -1;
  bool operator==(const Dots&) const = default;
};
// The dots of `set` (pure; `on` set).
Dots DotsOf(const SetBlock::SetState& set);
// The last dots the hook published (any thread); off during a fight.
Dots CurrentDots();
}  // namespace Orca::UX::RankedSet
