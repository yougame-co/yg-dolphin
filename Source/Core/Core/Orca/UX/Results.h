// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/SetBlock.h"

namespace Core
{
class CPUThreadGuard;
}

// A matchmade game's result, read from the game's own memory (ORCA.md "Matchmaking"): which port
// won, and the stage, characters and stocks, for the room's game reports. Read only: nothing here
// writes emulated memory (except QueueRules below, a host's own solo frames before any keyframe).
//
// Found under the harness (ORCA_UX_PROBE dumps, Brawl rev 2 and Project+ alike, which run the same
// executable and scene code), from g_GameGlobal (0x805A00E0 -> 0x90181300):
//   +0x08 gmGlobalModeMelee (0x90180F20): +0x1B the stage id, +0x20 the time limit in frames;
//         +0x98 the players' init data, 4 x 0x5C: +0x00 character, +0x01 player type (0 human,
//         1 CPU, 3 none), +0x04 starting stocks (0xFF in a timed match)
//   +0x18 the result info (0x9017F420), filled as the fight scene ends (before the results scene's
//         first frame):
//         +0x01 u8 the rules' mode (0 time, 1 stock); +0x04 u32 time limit (minutes); +0x08 u32
//         stocks; +0x0C u16 the stage id; +0x0E u16 how the fight ended (1 normally, by stocks or
//         by time; 2 a "no contest", the pause menu's L+R+A+Start); +0x1378 u8 the decision
//         (BrawlHeaders' gmResultInfo::DecisionKind: 1 time up, 2 a win, 9 no contest)
//         one record per port at +0x24 + port * 0x2AC: +0x00 u8 character, +0x01 u8 player type
//         (as above), +0x0A u8 stocks left (0xFF in a timed match), +0x0E u8 place (0 the
//         winner; a 1v1's loser 1), +0x10 u32 KOs, +0x14 u32 falls, +0x18 u16 self-destructs
//   +0x1C the rules (0x9017F360): +0x02 u8 mode (0 time, 1 stock), +0x03 u8 a timed match's
//         minutes, +0x04 u8 stocks, +0x08 u8 a stock match's time limit in minutes (0 none),
//         +0x0C u8 item frequency (0 none)
// The scene names (the scene manager 0x805A0060 +4 -> +0 name): "scMelee" the fight,
// "scMemoryChange" between scenes, "scVsResult" the results screen.
namespace Orca::UX
{
class GuestMemory;

struct PortResult
{
  bool present = false;  // a player (human or CPU) on this port
  bool human = false;
  int character = -1;
  int stocks = -1;  // stocks left; -1 in a timed match
  int place = -1;   // 0 the winner
  bool operator==(const PortResult&) const = default;
};

struct ResultBlock
{
  int mode = -1;  // 0 time, 1 stock
  int stage = -1;
  // +0x0E..+0x0F: read as how the fight ended (1 normally, 2 a no contest) by the beta; it is the
  // count of players tied for first (BrawlHeaders' m_numWinners at +0x0F).
  int end = 0;
  int decision = 0;  // gmResultInfo +0x1378: 1 time up, 2 a KO win, 9 no contest (0: not read)
  std::array<PortResult, 4> ports{};
  bool operator==(const ResultBlock&) const = default;
};

struct Reading
{
  enum class Scene : u8
  {
    Other,
    Fight,
    Between,
    Results,
  };
  Scene scene = Scene::Other;
  // In the results scene only.
  ResultBlock block;
  // A ranked match block's set (RankedSet.h), at every boundary of a ranked queue match.
  std::optional<SetBlock::SetState> set;
  bool operator==(const Reading&) const = default;
};

// One frame boundary's reading (reads only).
Reading ReadResults(const GuestMemory& memory);

// One game, as both players' Orcas report it.
struct GameResult
{
  enum class Kind
  {
    Win,
    Draw,
    Void,
  };
  Kind kind = Kind::Void;
  // Win: the winning port (0-based; seat s plays port s).
  int winner_port = -1;
  // Void: why ("nocontest", "ports").
  std::string why;
  // The detail (Win and Draw): the frame of the first results-scene reading, the stage, ports 1
  // and 2's characters and stocks left (-1 in a timed match), and whether the fight ran out of
  // time.
  int frame = -1;
  int stage = -1;
  std::array<int, 2> characters{-1, -1};
  std::array<int, 2> stocks{-1, -1};
  bool timeout = false;
  // A ranked set's game (from the match block): its fight's first frame (the report's id, the same
  // on both machines), its number in the set, how it was decided (SetBlock::How), a tiebreak
  // game, each port's ledge grabs; and, for the game that ended the set, the set's end (1 won by
  // `set_winner`, 2 void).
  int start = -1;
  int number = 0;
  int how = 0;
  bool tiebreak = false;
  std::array<int, 2> ledge{-1, -1};
  int set_done = 0;
  int set_winner = -1;
  bool operator==(const GameResult&) const = default;

  // {"f":F,"st":S,"c":[c1,c2],"s":[s1,s2],"to":0|1}: integers only, port order, this key order; a
  // ranked set's game adds "k":how,"lg":[l1,l2],"tb":0|1 (F is then its fight's first frame).
  std::string DetailJson() const;
};

// A ranked set's game as the match block recorded it (index `index` of `set`).
GameResult FromRecord(const SetBlock::SetState& set, int index);

// A results screen's verdict: exactly two human players on ports 1 and 2 (else void "ports"), a
// fight that ended normally (else void "nocontest"), one of them placed first (a win) or both (a
// draw).
GameResult Judge(const ResultBlock& block, int frame);

// Follows the readings of the frames as they become final, the same on every machine: a game is a
// fight scene entered at a frame after `plug_frame` (the opponent's plug frame, the same on both
// machines, so a fight running when the joiner dropped in never counts) and ended by the results
// scene (a result) or by any other scene (void "nocontest").
class ResultsTracker
{
public:
  // A frame boundary's reading: `frame` is the frame about to run (the frame hook's), so the
  // reading is of the state after the frames before it. A re-run's reading replaces the first
  // run's. `resyncs` is Events::Resyncs(): when it changes the game stopped following on from what
  // was read (a keyframe loaded, back to solo play), and everything stored is forgotten.
  void Store(int frame, const Reading& reading, u64 resyncs);
  // Readings of frames through `confirmed` are final (their inputs were every player's own, and no
  // re-run is pending): the games they finish, in order.
  std::vector<GameResult> Confirm(int confirmed, int plug_frame);
  void Reset();
  // The scene of the last final reading (none since the last reset), and of the newest reading
  // stored (a first run's guess until it is final).
  std::optional<Reading::Scene> FinalScene() const;
  std::optional<Reading::Scene> LatestScene() const;

private:
  std::map<int, Reading> m_pending;
  u64 m_resyncs = 0;
  bool m_started = false;
  // The last final reading.
  bool m_have_last = false;
  int m_last_frame = -1;
  Reading m_last;
  // A counted fight is on (or between it and its results screen).
  bool m_in_game = false;
  bool m_after_fight = false;
  // A ranked match block's set: how many of its games were seen (at a final reading) and whether
  // that count is known yet (the first final reading after a reset only sets it).
  bool m_have_set = false;
  int m_set_games = 0;
};

// The process's tracker (CPU thread only).
ResultsTracker& Tracker();

// The frame hook's part: stores this boundary's reading (first runs and re-runs).
void ReadResultsFrame(const Core::CPUThreadGuard& guard, int frame);

// Whether the running profile's results are read as above: Brawl rev 2, and Project+ (its
// launcher boots Brawl rev 2's executable).
bool ResultsVerified();

// A host that takes a matchmade room in Brawl presets its rules: 3 stocks, 8 minutes, items off
// (ORCA.md "Matchmaking"). Requested at that boundary; written at the next frame hook while the
// game is alone, before any keyframe of it (a joiner loads them with the host's state). Project+
// keeps its own rules.
void RequestQueueRules();
void ApplyQueueRules(const Core::CPUThreadGuard& guard, bool alone);
// The rules' bytes as written (exposed for tests): returns how many bytes changed.
int WriteQueueRules(GuestMemory& memory);
}  // namespace Orca::UX
