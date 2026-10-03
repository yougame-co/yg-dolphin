// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/RankedSet.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/Probe.h"

namespace Orca::UX::RankedSet
{
namespace
{
using MatchBlock::GameRecord;
using MatchBlock::How;
using MatchBlock::NO_PORT;
using MatchBlock::Ruleset;
using MatchBlock::SetState;
using Scene = Reading::Scene;

constexpr u32 GAME_GLOBAL = 0x805A00E0;
constexpr u32 GLOBAL_SET_RULE = 0x1C;
constexpr u32 RULE_MODE = 0x02;
constexpr u32 RULE_STOCKS = 0x04;
constexpr u32 RULE_STOCK_MINUTES = 0x08;
constexpr u8 DECISION_TIME_UP = 1;

u32 SetRule(const GuestMemory& m)
{
  if (!m.Valid(GAME_GLOBAL))
    return 0;
  const u32 global = m.Read32(GAME_GLOBAL);
  if (global % 4 || !m.Valid(global + GLOBAL_SET_RULE))
    return 0;
  const u32 rule = m.Read32(global + GLOBAL_SET_RULE);
  return rule % 4 == 0 && m.Valid(rule) && m.Valid(rule + RULE_STOCK_MINUTES) ? rule : 0;
}

u8 Byte(int v)
{
  return v < 0 || v > 254 ? 0xFF : static_cast<u8>(v);
}

int LedgeLimit(bool tiebreak, u8 character)
{
  const bool mk = character == META_KNIGHT;
  return tiebreak ? (mk ? LEDGE_LIMIT_TIEBREAK_MK : LEDGE_LIMIT_TIEBREAK) :
                    (mk ? LEDGE_LIMIT_MK : LEDGE_LIMIT);
}

// The last published set (CurrentScoreLine): bit 31 a ranked set, then done (2 bits), tiebreak,
// the wins, the game number.
std::atomic<u32> s_published{0};
std::atomic<int> s_local_port{0};
// SetRoomMode's wish: -1 nothing asked yet, else a MatchBlock::Mode.
std::atomic<int> s_room_mode{-1};
}  // namespace

std::optional<GameRecord> Judge(Ruleset ruleset, const SetState& set, const ResultBlock& block,
                                u8 decision)
{
  const auto& p = block.ports;
  if (block.end != 1 || !p[0].present || !p[1].present || !p[0].human || !p[1].human ||
      p[2].present || p[3].present)
  {
    return std::nullopt;
  }
  GameRecord r;
  r.start = set.fight_start;
  r.stage = Byte(block.stage);
  r.tiebreak = set.tiebreak;
  r.ledge = set.ledge;
  for (int i = 0; i < 2; ++i)
  {
    r.characters[i] = Byte(p[i].character);
    r.stocks[i] = Byte(p[i].stocks);
  }
  const bool snapped = set.fight_flags & MatchBlock::FIGHT_TIMEUP;
  const bool sudden = set.fight_flags & MatchBlock::FIGHT_SUDDEN;
  r.timeout = snapped || decision == DECISION_TIME_UP;
  const bool first0 = p[0].place == 0, first1 = p[1].place == 0;
  const auto win = [&](int port, How how) {
    r.winner = static_cast<u8>(port);
    r.how = how;
    return r;
  };
  const auto tie = [&] {
    r.winner = NO_PORT;
    r.how = How::Tie;
    return r;
  };
  if (ruleset != Ruleset::Brawl)
  {
    // Project+ decides time-outs itself (stocks, then percent; an exact tie gets overtime).
    if (first0 != first1)
      return win(first0 ? 0 : 1, How::Results);
    if (first0 && first1)
      return tie();
    return std::nullopt;
  }
  // A sudden death the clock wasn't seen to cause: a simultaneous last-stock loss, or a time-out
  // whose stocks and percent weren't read. Either way the tiebreak game decides.
  if (sudden && !snapped)
    return tie();
  if (r.timeout)
  {
    // The ledge-grab limit first: exactly one player over it loses.
    const bool over0 = set.ledge[0] > LedgeLimit(set.tiebreak, r.characters[0]);
    const bool over1 = set.ledge[1] > LedgeLimit(set.tiebreak, r.characters[1]);
    if (over0 != over1)
      return win(over0 ? 1 : 0, How::Ledge);
    // Then stocks, then percent, as they were when time ran out (else the result info's stocks:
    // with no sudden death they are the same).
    std::array<int, 2> stocks{p[0].stocks, p[1].stocks};
    if (snapped)
      stocks = {set.snap_stocks[0], set.snap_stocks[1]};
    if (stocks[0] >= 0 && stocks[1] >= 0 && stocks[0] != stocks[1])
      return win(stocks[0] > stocks[1] ? 0 : 1, How::Stocks);
    if (snapped && set.snap_percent[0] != set.snap_percent[1])
      return win(set.snap_percent[0] < set.snap_percent[1] ? 0 : 1, How::Percent);
    if (!snapped && first0 != first1)
      return win(first0 ? 0 : 1, How::Results);
    return tie();
  }
  if (first0 != first1)
    return win(first0 ? 0 : 1, How::Results);
  if (first0 && first1)
    return tie();
  return std::nullopt;
}

SetState Step(SetState s, Ruleset ruleset, const Facts& f, u32 frame)
{
  if (s.done)
  {
    // The set is over: nothing more is followed.
    if (s.fight)
    {
      s.fight = false;
      s.fight_flags = 0;
    }
    return s;
  }
  switch (f.scene)
  {
  case Scene::Fight:
    if (!s.fight)
    {
      s.fight = true;
      s.fight_flags = 0;
      s.fight_start = frame;
      s.ledge = {0, 0};
      s.snap_stocks = {0, 0};
      s.snap_percent = {0, 0};
      s.on_ledge = {0, 0};
    }
    else if (s.fight_flags & MatchBlock::FIGHT_BETWEEN)
    {
      // Back into a fight from between scenes: sudden death, the same game.
      s.fight_flags = static_cast<u8>((s.fight_flags & ~MatchBlock::FIGHT_BETWEEN) |
                                      MatchBlock::FIGHT_SUDDEN);
    }
    if (f.live.valid && !(s.fight_flags & MatchBlock::FIGHT_SUDDEN))
    {
      for (int i = 0; i < 2; ++i)
      {
        if (f.live.on_ledge[i] && !s.on_ledge[i] && s.ledge[i] < 255)
          ++s.ledge[i];
        s.on_ledge[i] = f.live.on_ledge[i] ? 1 : 0;
      }
      if (f.live.time_up && !(s.fight_flags & MatchBlock::FIGHT_TIMEUP))
      {
        s.fight_flags |= MatchBlock::FIGHT_TIMEUP;
        s.snap_stocks = f.live.stocks;
        s.snap_percent = f.live.percent;
      }
    }
    break;
  case Scene::Between:
    if (s.fight)
      s.fight_flags |= MatchBlock::FIGHT_BETWEEN;
    break;
  case Scene::Results:
  {
    if (!s.fight)
      break;
    const std::optional<GameRecord> r = Judge(ruleset, s, f.block, f.decision);
    s.fight = false;
    s.fight_flags = 0;
    s.on_ledge = {0, 0};
    if (!r)
      break;
    if (s.games < MatchBlock::MAX_RECORDS)
      s.records[s.games] = *r;
    ++s.games;
    if (r->winner != NO_PORT)
    {
      const int w = r->winner;
      ++s.wins[w];
      s.last_winner = r->winner;
      s.tiebreak = false;
      s.tb_streak = 0;
      if (s.wins[w] >= WINS)
      {
        s.done = 1;
        s.winner = r->winner;
      }
    }
    else
    {
      ++s.tb_streak;
      // Brawl plays a tiebreak game; Project+'s own overtime leaves a tie only by accident.
      s.tiebreak = ruleset == Ruleset::Brawl;
      if (s.tb_streak >= TIEBREAK_STREAK_LIMIT)
        s.done = 2;
    }
    if (!s.done && s.games >= MatchBlock::MAX_RECORDS)
      s.done = 2;
    if (s.done == 2)
    {
      s.winner = NO_PORT;
      s.tiebreak = false;
    }
    break;
  }
  case Scene::Other:
    if (s.fight)
    {
      // The fight ended without its results screen: not a game.
      s.fight = false;
      s.fight_flags = 0;
      s.on_ledge = {0, 0};
    }
    break;
  }
  return s;
}

int WriteSetRules(GuestMemory& memory, const SetState& set)
{
  const u32 rule = SetRule(memory);
  if (!rule)
    return 0;
  int changed = 0;
  const auto put = [&](u32 offset, u8 value) {
    if (memory.Read8(rule + offset) == value)
      return;
    memory.Write8(rule + offset, value);
    ++changed;
  };
  put(RULE_MODE, 1);
  put(RULE_STOCKS, set.tiebreak ? 1 : 3);
  put(RULE_STOCK_MINUTES, set.tiebreak ? 3 : 8);
  return changed;
}

Live ReadLiveFight(const GuestMemory& memory)
{
  // Not located yet: the fight's clock, stocks, percent and ledge state (ORCA.md "Ranked sets").
  (void)memory;
  return {};
}

Facts ReadFacts(const GuestMemory& memory)
{
  Facts f;
  const Reading r = ReadResults(memory);
  f.scene = r.scene;
  f.block = r.block;
  f.decision = static_cast<u8>(r.block.decision);
  if (f.scene == Scene::Fight)
    f.live = ReadLiveFight(memory);
  return f;
}

void Frame(const Core::CPUThreadGuard& guard, int frame)
{
  if (!ResultsVerified())
    return;
  GuardMemory memory(guard);
  const std::optional<MatchBlock::Header> header = MatchBlock::ReadHeader(memory);
  if (!header || header->mode != MatchBlock::Mode::Ranked)
  {
    s_published.store(0, std::memory_order_relaxed);
    return;
  }
  const Facts facts = ReadFacts(memory);
  const SetState before = MatchBlock::ReadSet(memory);
  const SetState after = Step(before, header->ruleset, facts, static_cast<u32>(frame));
  if (after != before)
  {
    MatchBlock::WriteSet(memory, after);
    if (after.games != before.games)
    {
      const GameRecord& r = after.records[std::min(after.games, u8(MatchBlock::MAX_RECORDS)) - 1];
      NOTICE_LOG_FMT(ROLLBACK,
                     "Ranked set: game {} at frame {} (fight from {}): winner {}, how {}, "
                     "ledge {}/{}, set {}-{}{}",
                     after.games, frame, r.start, r.winner == NO_PORT ? 0 : r.winner + 1,
                     static_cast<int>(r.how), r.ledge[0], r.ledge[1], after.wins[0],
                     after.wins[1],
                     after.done == 1 ? " (over)" :
                     after.done == 2 ? " (void)" :
                     after.tiebreak  ? " (tiebreak next)" :
                                       "");
    }
  }
  if (header->ruleset == Ruleset::Brawl && facts.scene != Scene::Fight)
    WriteSetRules(memory, after);
  const bool fighting = facts.scene == Scene::Fight || facts.scene == Scene::Between;
  const u32 packed = 0x80000000u | (fighting ? 1u << 30 : 0) | (u32(after.done & 3) << 24) |
                     (after.tiebreak ? 1u << 23 : 0) |
                     (u32(after.wins[0]) << 16) | (u32(after.wins[1]) << 8) |
                     u32(std::min(after.GameNumber(), 255));
  s_published.store(packed, std::memory_order_relaxed);
}

void SetRoomMode(MatchBlock::Mode mode)
{
  s_room_mode.store(static_cast<int>(mode));
}

void ApplyRoomMode(const Core::CPUThreadGuard& guard, bool alone)
{
  // Test only, ORCA_TEST_MATCH_BLOCK=ranked|casual: a queue room's header with no room (harness
  // runs, a dev game's room), as if this game hosted one.
  static const int s_test = [] {
    const char* v = std::getenv("ORCA_TEST_MATCH_BLOCK");
    if (!v)
      return -1;
    return std::strcmp(v, "ranked") == 0 ? static_cast<int>(MatchBlock::Mode::Ranked) :
           std::strcmp(v, "casual") == 0 ? static_cast<int>(MatchBlock::Mode::Casual) :
                                           -1;
  }();
  if (s_test >= 0 && s_room_mode.load() < 0 && TestKnobsAllowed())
  {
    WARN_LOG_FMT(ROLLBACK, "Match block: TEST header from ORCA_TEST_MATCH_BLOCK");
    s_room_mode.store(s_test);
  }
  const int wanted = s_room_mode.load();
  if (wanted < 0 || !alone || !ResultsVerified())
    return;
  GuardMemory memory(guard);
  const std::optional<MatchBlock::Header> have = MatchBlock::ReadHeader(memory);
  const auto mode = static_cast<MatchBlock::Mode>(wanted);
  if (mode == MatchBlock::Mode::None)
  {
    if (!have)
      return;
    const int changed = MatchBlock::Clear(memory);
    NOTICE_LOG_FMT(ROLLBACK, "Match block: cleared ({} byte(s))", changed);
    return;
  }
  const Orca::Profile* profile = Orca::ActiveProfile();
  MatchBlock::Header header;
  header.mode = mode;
  header.ruleset = profile && profile->IsLauncher() ? Ruleset::PPlus : Ruleset::Brawl;
  if (have == header)
    return;
  const int changed = MatchBlock::WriteHeader(memory, header);
  NOTICE_LOG_FMT(ROLLBACK, "Match block: header {} {} ({} byte(s))",
                 mode == MatchBlock::Mode::Ranked ? "ranked" : "casual",
                 header.ruleset == Ruleset::PPlus ? "Project+" : "Brawl", changed);
}

std::string ScoreLine(const SetState& set, int local_port)
{
  const int me = local_port == 1 ? 1 : 0;
  const int mine = set.wins[me], theirs = set.wins[1 - me];
  if (set.done == 2)
    return fmt::format("Set void · {}–{}", mine, theirs);
  if (set.done)
    return fmt::format("Set over · {}–{}", mine, theirs);
  if (set.tiebreak)
    return fmt::format("Tiebreak game · {}–{}", mine, theirs);
  return fmt::format("Game {} · {}–{}", set.GameNumber(), mine, theirs);
}

std::string CurrentScoreLine()
{
  const u32 packed = s_published.load(std::memory_order_relaxed);
  // Only on the menus (the character and stage select, the results screen), never over a fight.
  if (!(packed & 0x80000000u) || (packed & (1u << 30)))
    return "";
  SetState s;
  s.done = static_cast<u8>((packed >> 24) & 3);
  s.tiebreak = packed & (1u << 23);
  s.wins = {static_cast<u8>((packed >> 16) & 0x7F), static_cast<u8>((packed >> 8) & 0xFF)};
  return ScoreLine(s, s_local_port.load(std::memory_order_relaxed));
}

void SetLocalPort(int port)
{
  s_local_port.store(port, std::memory_order_relaxed);
}
}  // namespace Orca::UX::RankedSet
