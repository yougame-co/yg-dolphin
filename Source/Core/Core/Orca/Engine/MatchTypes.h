// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>

#include "Common/CommonTypes.h"

// Types the session and a game module share (Orca split, S2 commit 5): what the module reports and
// the session acts on. They were the Brawl modules' own types; the modules keep using-declarations.
namespace Orca::Engine
{
// Where a frame's game is, as the results tracker reads it (Brawl: Results.h Reading::Scene).
enum class SceneKind : u8
{
  Other,
  Fight,
  Between,
  Results,
};

// One game's result, as both players report it.
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
  // Win and Draw: first results-scene frame, stage, ports 1 and 2's characters and stocks left
  // (-1 timed), and whether time ran out.
  int frame = -1;
  int stage = -1;
  std::array<int, 2> characters{-1, -1};
  std::array<int, 2> stocks{-1, -1};
  bool timeout = false;
  // Ranked set games only (from the match block): the fight's first frame (the report id, same on
  // both machines), game number, how it was decided (SetBlock::How), tiebreak, each port's ledge
  // grabs, and for the deciding game the set's end (1 won by `set_winner`, 2 void).
  int start = -1;
  int number = 0;
  int how = 0;
  bool tiebreak = false;
  std::array<int, 2> ledge{-1, -1};
  int set_done = 0;
  int set_winner = -1;
  bool operator==(const GameResult&) const = default;

  // {"f":F,"st":S,"c":[c1,c2],"s":[s1,s2],"to":0|1}: integers only, port order, this key order. A
  // ranked set game adds "k":how,"lg":[l1,l2],"tb":0|1 (F is then the fight's first frame).
  std::string DetailJson() const;
};
}  // namespace Orca::Engine
