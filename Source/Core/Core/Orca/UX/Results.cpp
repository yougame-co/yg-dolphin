// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Results.h"

#include <atomic>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/OnlineMenu.h"

namespace Orca::UX
{
namespace
{
constexpr u32 GAME_GLOBAL = 0x805A00E0;
constexpr u32 GLOBAL_RESULT_INFO = 0x18;
constexpr u32 GLOBAL_SET_RULE = 0x1C;
constexpr u32 RESULT_MODE = 0x01;
constexpr u32 RESULT_STAGE = 0x0C;
constexpr u32 RESULT_END = 0x0E;
constexpr u32 RESULT_PORTS = 0x24;
constexpr u32 RESULT_PORT_SIZE = 0x2AC;
constexpr u32 PORT_CHARACTER = 0x00;
constexpr u32 PORT_TYPE = 0x01;
constexpr u32 PORT_STOCKS = 0x0A;
constexpr u32 PORT_PLACE = 0x0E;
constexpr u8 TYPE_HUMAN = 0;
constexpr u8 TYPE_NONE = 3;
// The rules (gmSetRule): what the Rules menu sets.
constexpr u32 RULE_MODE = 0x02;
constexpr u32 RULE_STOCKS = 0x04;
constexpr u32 RULE_STOCK_MINUTES = 0x08;
constexpr u32 RULE_ITEMS = 0x0C;
// How many readings may wait for their frames to be final: far more than a rollback window or a
// joiner's catch-up between two confirmations (a confirmation comes every boundary).
constexpr size_t MAX_PENDING = 4096;

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

// The struct a g_GameGlobal field points to, or 0.
u32 GlobalField(const GuestMemory& m, u32 field)
{
  if (!Pointer(m, GAME_GLOBAL))
    return 0;
  const u32 global = m.Read32(GAME_GLOBAL);
  if (!Pointer(m, global + field))
    return 0;
  const u32 p = m.Read32(global + field);
  return Pointer(m, p) ? p : 0;
}

std::atomic<bool> s_rules_pending{false};
}  // namespace

std::string GameResult::DetailJson() const
{
  return fmt::format("{{\"f\":{},\"st\":{},\"c\":[{},{}],\"s\":[{},{}],\"to\":{}}}", frame, stage,
                     characters[0], characters[1], stocks[0], stocks[1], timeout ? 1 : 0);
}

Reading ReadResults(const GuestMemory& m)
{
  Reading r;
  const std::string scene = ReadSceneName(m);
  if (scene == "scMelee")
    r.scene = Reading::Scene::Fight;
  else if (scene == "scMemoryChange")
    r.scene = Reading::Scene::Between;
  else if (scene == "scVsResult")
    r.scene = Reading::Scene::Results;
  if (r.scene != Reading::Scene::Results)
    return r;
  const u32 info = GlobalField(m, GLOBAL_RESULT_INFO);
  if (!info || !m.Valid(info + RESULT_PORTS + 4 * RESULT_PORT_SIZE))
    return r;
  r.block.mode = m.Read8(info + RESULT_MODE);
  r.block.stage = m.Read16(info + RESULT_STAGE);
  r.block.end = m.Read16(info + RESULT_END);
  for (u32 port = 0; port < 4; ++port)
  {
    const u32 rec = info + RESULT_PORTS + port * RESULT_PORT_SIZE;
    PortResult& p = r.block.ports[port];
    const u8 type = m.Read8(rec + PORT_TYPE);
    p.present = type != TYPE_NONE;
    p.human = type == TYPE_HUMAN;
    if (!p.present)
      continue;
    p.character = m.Read8(rec + PORT_CHARACTER);
    const u8 stocks = m.Read8(rec + PORT_STOCKS);
    p.stocks = stocks == 0xFF ? -1 : stocks;
    p.place = m.Read8(rec + PORT_PLACE);
  }
  return r;
}

GameResult Judge(const ResultBlock& block, int frame)
{
  GameResult g;
  g.frame = frame;
  if (block.end != 1)
  {
    g.why = "nocontest";
    return g;
  }
  const auto& p = block.ports;
  if (!p[0].present || !p[1].present || !p[0].human || !p[1].human || p[2].present ||
      p[3].present)
  {
    g.why = "ports";
    return g;
  }
  g.stage = block.stage;
  for (int i = 0; i < 2; ++i)
  {
    g.characters[i] = p[i].character;
    g.stocks[i] = p[i].stocks;
  }
  // Out of time: a timed match always; a stock match when nobody ran out of stocks.
  g.timeout = block.mode == 0 || (p[0].stocks > 0 && p[1].stocks > 0);
  const bool first0 = p[0].place == 0, first1 = p[1].place == 0;
  if (first0 && first1)
  {
    g.kind = GameResult::Kind::Draw;
  }
  else if (first0 || first1)
  {
    g.kind = GameResult::Kind::Win;
    g.winner_port = first0 ? 0 : 1;
  }
  else
  {
    g.why = "nocontest";
  }
  return g;
}

void ResultsTracker::Reset()
{
  m_pending.clear();
  m_have_last = false;
  m_last_frame = -1;
  m_last = {};
  m_in_game = false;
  m_after_fight = false;
}

void ResultsTracker::Store(int frame, const Reading& reading, u64 resyncs)
{
  if (!m_started || resyncs != m_resyncs)
  {
    Reset();
    m_started = true;
    m_resyncs = resyncs;
  }
  // A frame already final here is never stored again (a rollback never reaches a confirmed frame).
  if (m_have_last && frame <= m_last_frame)
    return;
  m_pending[frame] = reading;
  while (m_pending.size() > MAX_PENDING)
    m_pending.erase(m_pending.begin());
}

std::vector<GameResult> ResultsTracker::Confirm(int confirmed, int plug_frame)
{
  using Scene = Reading::Scene;
  std::vector<GameResult> out;
  for (auto it = m_pending.begin(); it != m_pending.end() && it->first <= confirmed;
       it = m_pending.erase(it))
  {
    const int frame = it->first;
    const Reading& r = it->second;
    // Frames follow on from the last one: a gap (frames this machine never read) ends what was
    // being followed.
    const bool follows = m_have_last && frame == m_last_frame + 1;
    if (!follows)
    {
      m_in_game = false;
      m_after_fight = false;
    }
    const Scene before = follows ? m_last.scene : Scene::Other;
    switch (r.scene)
    {
    case Scene::Fight:
      if (before != Scene::Fight)
      {
        if (m_in_game && m_after_fight)
        {
          // Back into the fight from between scenes (sudden death): the same game.
          m_after_fight = false;
        }
        else
        {
          // A game: the fight starts here, with the frame before it read on both machines (after
          // the opponent's plug frame).
          m_in_game = follows && frame - 1 > plug_frame;
          m_after_fight = false;
        }
      }
      break;
    case Scene::Between:
      if (m_in_game && before == Scene::Fight)
        m_after_fight = true;
      break;
    case Scene::Results:
      if (m_in_game)
      {
        out.push_back(Judge(r.block, frame));
        m_in_game = false;
        m_after_fight = false;
      }
      break;
    case Scene::Other:
      if (m_in_game)
      {
        // The fight ended without its results screen.
        GameResult g;
        g.frame = frame;
        g.why = "nocontest";
        out.push_back(g);
        m_in_game = false;
        m_after_fight = false;
      }
      break;
    }
    m_last = r;
    m_last_frame = frame;
    m_have_last = true;
  }
  return out;
}

ResultsTracker& Tracker()
{
  static ResultsTracker tracker;
  return tracker;
}

bool ResultsVerified()
{
  const Orca::Profile* profile = Orca::ActiveProfile();
  if (!profile || profile->revision != 2)
    return false;
  // Project+'s launcher boots Brawl rev 2's executable: the same scenes and result info.
  return profile->IsLauncher() ? profile->disc == "RSBE01" : profile->game_id == "RSBE01";
}

void ReadResultsFrame(const Core::CPUThreadGuard& guard, int frame)
{
  if (!ResultsVerified())
    return;
  GuardMemory memory(guard);
  Tracker().Store(frame, ReadResults(memory), Orca::Events::Resyncs());
}

void RequestQueueRules()
{
  const Orca::Profile* profile = Orca::ActiveProfile();
  if (profile && !profile->IsLauncher() && profile->game_id == "RSBE01" && profile->revision == 2)
    s_rules_pending = true;
}

int WriteQueueRules(GuestMemory& memory)
{
  const u32 rule = GlobalField(memory, GLOBAL_SET_RULE);
  if (!rule || !memory.Valid(rule + RULE_ITEMS))
    return 0;
  int changed = 0;
  const auto set = [&](u32 offset, u8 value) {
    if (memory.Read8(rule + offset) == value)
      return;
    memory.Write8(rule + offset, value);
    ++changed;
  };
  set(RULE_MODE, 1);
  set(RULE_STOCKS, 3);
  set(RULE_STOCK_MINUTES, 8);
  set(RULE_ITEMS, 0);
  return changed;
}

void ApplyQueueRules(const Core::CPUThreadGuard& guard, bool alone)
{
  // Only at the boundary after the host took its room: a later write could land after a keyframe
  // a friend replays from.
  if (!s_rules_pending.exchange(false))
    return;
  if (!alone)
  {
    WARN_LOG_FMT(ROLLBACK, "Matchmaking: no rules preset: the game wasn't alone");
    return;
  }
  GuardMemory memory(guard);
  const int changed = WriteQueueRules(memory);
  NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the rules preset (3 stocks, 8:00, items off), {} byte(s) "
                           "changed",
                 changed);
}
}  // namespace Orca::UX
