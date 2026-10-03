// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/RankedPPlus.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/UX/MatchBlock.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/OnlineMenu.h"
#include "Core/Orca/UX/RankedSteps.h"
#include "Core/Orca/UX/Results.h"

namespace Orca::UX::RankedPPlus
{
// Project+ v3.2, pf/stage/switch/Switch03.rss ("2024 Proposed"): 0x320 bytes, zeros after 0x182.
const u8 kSwitch03[RSS_EXDATA_SIZE] = {
    0x00, 0x00, 0x00, 0x03, 0x01, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x1C, 0x05, 0x0B,
    0x23, 0x08, 0x1A, 0x00, 0x28, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1B, 0x01, 0x38, 0x24, 0x3B, 0x04, 0x02, 0x2D, 0x31, 0x20, 0x0C, 0x39,
    0x15, 0x06, 0x33, 0x07, 0x32, 0x21, 0x09, 0x1F, 0x3D, 0x2E, 0x27, 0x35, 0x0A, 0x18, 0x0D, 0x36,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x0F, 0x3C, 0x10,
    0x34, 0x25, 0x3E, 0x0E, 0x22, 0x19, 0x14, 0x12, 0x26, 0x11, 0x13, 0x17, 0x16, 0x37, 0x2F, 0x1D,
    0x3A, 0x2C, 0x1B, 0x1E, 0x30, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x04, 0x04, 0x05, 0x05, 0x06, 0x06,
    0x07, 0x07, 0x08, 0x08, 0x09, 0x09, 0x33, 0x0A, 0x49, 0x2C, 0x0C, 0x0C, 0x0D, 0x0D, 0x0E, 0x0E,
    0x13, 0x0F, 0x14, 0x10, 0x15, 0x11, 0x16, 0x12, 0x17, 0x13, 0x18, 0x14, 0x19, 0x15, 0x1C, 0x16,
    0x1D, 0x17, 0x1E, 0x18, 0x1F, 0x19, 0x20, 0x1A, 0x21, 0x1B, 0x22, 0x1C, 0x23, 0x1D, 0x24, 0x1E,
    0x43, 0x26, 0x29, 0x32, 0x2A, 0x33, 0x47, 0x2A, 0x2C, 0x35, 0x2D, 0x36, 0x2F, 0x37, 0x30, 0x38,
    0x31, 0x39, 0x32, 0x3A, 0x2E, 0x3B, 0xFF, 0x64, 0xFF, 0x64, 0x37, 0x3C, 0x40, 0x23, 0x41, 0x24,
    0x42, 0x25, 0x25, 0x1F, 0x44, 0x27, 0x45, 0x28, 0x46, 0x29, 0x2B, 0x34, 0x48, 0x2B, 0x0B, 0x0B,
    0x4A, 0x2D, 0x4B, 0x2E, 0x4C, 0x2F, 0x4D, 0x30, 0x4E, 0x31, 0x4F, 0x3D, 0x50, 0x3E, 0x51, 0x3F,
    0x52, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

namespace
{
using Ranked::Plan;
using Ranked::StageMask;
using Ranked::Step;
using Ranked::StepKind;
using Rollback::InputGate::Mask;

constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 SCENE_SSS_TASK = 0x3AC;
constexpr u32 GAME_GLOBAL = 0x805A00E0;
constexpr u32 GLOBAL_MODE_MELEE = 0x08;
constexpr u32 MODE_MELEE_STAGE = 0x1B;
// The preset's page 0: its stage count, the stage slots, and the slot table (slot -> kind).
constexpr u32 RSS_PAGE0_COUNT = RSS_EXDATA + 0x3C;
constexpr u32 RSS_PAGE0_SLOTS = RSS_EXDATA + 0x3D;
constexpr u32 RSS_SLOT_KINDS = RSS_EXDATA + 0x104;
constexpr int MAX_PAGE_STAGES = 31;

// The 2024 Proposed list's stages by Project+ stage kind (pf/stage/stageslot/<kind>.asl), in the
// ruleset's order (RankedSteps.cpp PPlus2024).
constexpr std::array<u8, 9> LEGAL_KINDS{
    0x01,  // Battlefield
    0x2E,  // Pokémon Stadium 2
    0x21,  // Smashville
    0x04,  // Luigi's Mansion
    0x09,  // Temple of Time
    0x23,  // Green Hill Zone
    0x06,  // Bowser's Castle
    0x0C,  // Frigate Husk
    0x2D,  // Dream Land
};

constexpr StageMask Bit(int i)
{
  return static_cast<StageMask>(1u << i);
}

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

// The stage select task while the scene is scSelStage, else 0.
u32 StageSelectTask(const GuestMemory& m, const std::string& scene)
{
  if (scene != "scSelStage" || !Pointer(m, SCENE_MANAGER))
    return 0;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + 4))
    return 0;
  const u32 current = m.Read32(manager + 4);
  if (!Pointer(m, current + SCENE_SSS_TASK))
    return 0;
  const u32 task = m.Read32(current + SCENE_SSS_TASK);
  return Pointer(m, task) && m.Valid(task + SSS_CONTROLLER + 3) ? task : 0;
}

int ModeMeleeStage(const GuestMemory& m)
{
  if (!Pointer(m, GAME_GLOBAL))
    return -1;
  const u32 global = m.Read32(GAME_GLOBAL);
  if (!Pointer(m, global + GLOBAL_MODE_MELEE))
    return -1;
  const u32 mm = m.Read32(global + GLOBAL_MODE_MELEE);
  if (!Pointer(m, mm) || !m.Valid(mm + MODE_MELEE_STAGE))
    return -1;
  return m.Read8(mm + MODE_MELEE_STAGE);
}

bool IsRankedPPlus(const MatchBlock::State& s)
{
  return s.mode == MatchBlock::MODE_RANKED && s.ruleset == Ranked::RULESET_PPLUS_2024;
}

Ranked::SetView ViewOf(const MatchBlock::State& s)
{
  Ranked::SetView v;
  v.game = s.games + 1;
  v.last_winner = s.last_winner <= 1 ? s.last_winner : -1;
  v.coin = s.coin;
  v.won_on = s.won_on;
  v.characters = {s.characters[0] == MatchBlock::NONE ? -1 : s.characters[0],
                  s.characters[1] == MatchBlock::NONE ? -1 : s.characters[1]};
  return v;
}

// The stages shown struck: not in play this game, struck or banned so far, and while the pick's
// timer ran out, everything but its default.
StageMask ShownStruck(const Ranked::Ruleset& rules, const Plan& plan, const MatchBlock::State& s)
{
  StageMask shown = static_cast<StageMask>(rules.All() & ~plan.available);
  shown |= s.struck;
  if (s.step < plan.steps.size() && plan.steps[s.step].kind == StepKind::Pick)
    shown |= plan.pick_excluded;
  if (s.auto_pick && s.auto_stage < Ranked::MAX_STAGES)
    shown = static_cast<StageMask>(rules.All() & ~Bit(s.auto_stage));
  return shown;
}

// Page 0's strike bits for positions 0-30 (+2: high half, +4: low half).
u32 ReadPage0Bits(const GuestMemory& m)
{
  return u32(m.Read16(STAGE_STRIKE_TABLE + 2)) << 16 | m.Read16(STAGE_STRIKE_TABLE + 4);
}

int WriteIfDifferent16(GuestMemory& m, u32 address, u16 value)
{
  if (m.Read16(address) == value)
    return 0;
  m.Write16(address, value);
  return 2;
}

// The table the block calls for: page 0's struck stages (and any position that isn't a legal
// stage), every other page all struck. Returns bytes changed.
int WriteStrikeTable(GuestMemory& m, const std::vector<int>& page0, StageMask shown)
{
  if (!m.Valid(STAGE_STRIKE_TABLE + STRIKE_PAGES * 6 - 1))
    return 0;
  u32 bits = 0;
  for (size_t pos = 0; pos < page0.size(); ++pos)
  {
    const int legal = page0[pos];
    if (legal < 0 || (shown & Bit(legal)))
      bits |= 1u << pos;
  }
  int changed = 0;
  changed += WriteIfDifferent16(m, STAGE_STRIKE_TABLE + 0, 0);
  changed += WriteIfDifferent16(m, STAGE_STRIKE_TABLE + 2, static_cast<u16>(bits >> 16));
  changed += WriteIfDifferent16(m, STAGE_STRIKE_TABLE + 4, static_cast<u16>(bits));
  for (u32 page = 1; page < STRIKE_PAGES; ++page)
  {
    for (u32 half = 0; half < 3; ++half)
      changed += WriteIfDifferent16(m, STAGE_STRIKE_TABLE + page * 6 + half * 2, 0xFFFF);
  }
  return changed;
}

int PositionOf(const std::vector<int>& page0, int legal)
{
  for (size_t pos = 0; pos < page0.size(); ++pos)
  {
    if (page0[pos] == legal)
      return static_cast<int>(pos);
  }
  return -1;
}

// A new step starts now.
void StartStep(const Ranked::Ruleset& rules, const Plan& plan, MatchBlock::State& s, int frame)
{
  s.step_done = 0;
  s.auto_pick = false;
  s.auto_stage = MatchBlock::NONE;
  if (s.step < plan.steps.size())
  {
    s.active = plan.steps[s.step].port;
    s.deadline = static_cast<u32>(frame) + Ranked::StepFrames(rules, plan.steps[s.step]);
  }
  else
  {
    s.active = MatchBlock::NONE;
    s.deadline = 0;
  }
}

// The stage select's part of a boundary.
void StageSelect(GuestMemory& m, const Ranked::Ruleset& rules, MatchBlock::State& s, int frame,
                 const std::vector<int>& page0)
{
  if (s.flow == 0)
  {
    // The game's steps begin: nothing struck yet.
    s.flow = 1;
    s.step = 0;
    s.struck = 0;
    const Plan plan = Ranked::PlanGame(rules, ViewOf(s));
    StartStep(rules, plan, s, frame);
  }
  const Plan plan = Ranked::PlanGame(rules, ViewOf(s));
  if (plan.steps.empty())
    return;
  if (s.step >= plan.steps.size())
  {
    s.step = static_cast<u8>(plan.steps.size() - 1);
    StartStep(rules, plan, s, frame);
  }
  // A deadline from a frame numbering that isn't this one (never longer than a whole step).
  const Step& now = plan.steps[s.step];
  const u32 full = Ranked::StepFrames(rules, now);
  if (s.deadline > static_cast<u32>(frame) + full)
    s.deadline = static_cast<u32>(frame) + full;
  const bool expired = static_cast<u32>(frame) >= s.deadline;

  if (now.kind != StepKind::Pick)
  {
    // The active player's new strikes since the last boundary, on stages still allowed.
    const u32 bits = ReadPage0Bits(m);
    StageMask table = 0;
    for (size_t pos = 0; pos < page0.size() && pos < 31; ++pos)
    {
      if (page0[pos] >= 0 && (bits & (1u << pos)))
        table |= Bit(page0[pos]);
    }
    const StageMask fresh = table & Ranked::Allowed(plan, s.step, s.struck);
    const int owed = now.count - s.step_done;
    if (fresh && owed > 0)
    {
      const StageMask take = Ranked::FirstBits(fresh, owed);
      s.struck |= take;
      s.step_done = static_cast<u8>(s.step_done + Ranked::CountBits(take));
    }
    bool advance = s.step_done >= now.count;
    if (!advance && expired)
    {
      const StageMask defaults = Ranked::DefaultStrikes(plan, s.step, s.struck, s.step_done);
      s.struck |= defaults;
      advance = true;
    }
    if (advance)
    {
      ++s.step;
      StartStep(rules, plan, s, frame);
    }
  }
  else if (expired && !s.auto_pick)
  {
    const int pick = Ranked::DefaultPick(plan, s.step, s.struck);
    if (pick >= 0)
    {
      s.auto_pick = true;
      s.auto_stage = static_cast<u8>(pick);
    }
  }
}

// One boundary's set and flow, Project+ ranked. `task` is the stage select's (0 elsewhere).
void Step1(GuestMemory& m, MatchBlock::State& s, int frame, bool two_players,
           const std::string& scene, u32 task)
{
  const Ranked::Ruleset& rules = Ranked::PPlus2024();
  if (scene == "scVsResult" && s.fight)
  {
    const Reading r = ReadResults(m);
    const GameResult g = Judge(r.block, frame);
    if (g.kind == GameResult::Kind::Win && !s.done)
    {
      const int w = g.winner_port;
      const int stage = s.stage < Ranked::MAX_STAGES ? s.stage : LegalIndexOfKind(g.stage);
      if (s.games < MatchBlock::MAX_GAMES)
      {
        MatchBlock::GameRecord& rec = s.records[s.games];
        rec.winner = static_cast<u8>(w);
        rec.stage = stage >= 0 ? static_cast<u8>(stage) : MatchBlock::NONE;
        for (int p = 0; p < 2; ++p)
          rec.characters[p] =
              g.characters[p] >= 0 ? static_cast<u8>(g.characters[p]) : MatchBlock::NONE;
      }
      for (int p = 0; p < 2; ++p)
        s.characters[p] =
            g.characters[p] >= 0 ? static_cast<u8>(g.characters[p]) : MatchBlock::NONE;
      s.games = static_cast<u8>(std::min<int>(s.games + 1, 0xFF));
      s.score[w] = static_cast<u8>(s.score[w] + 1);
      s.last_winner = static_cast<u8>(w);
      if (stage >= 0)
        s.won_on[w] |= Bit(stage);
      if (s.score[w] >= rules.wins_needed)
      {
        s.done = true;
        s.set_winner = static_cast<u8>(w);
      }
    }
    s.fight = false;
    s.stage = MatchBlock::NONE;
  }
  if (scene == "scMelee" && s.flow == 1)
  {
    // The game's fight, from the stage the flow ended on.
    s.flow = 0;
    s.fight = true;
    const int legal = LegalIndexOfKind(ModeMeleeStage(m));
    s.stage = legal >= 0 ? static_cast<u8>(legal) : MatchBlock::NONE;
    s.step = 0;
    s.struck = 0;
    s.step_done = 0;
    s.active = MatchBlock::NONE;
    s.auto_pick = false;
    s.auto_stage = MatchBlock::NONE;
  }
  if (scene == "scSelctCharacter")
  {
    // Back on the characters: a flow left without a pick starts over at the next stage select, and
    // a fight that ended without its results screen counts for nothing.
    if (s.flow != 0 || s.fight)
    {
      s.flow = 0;
      s.fight = false;
      s.stage = MatchBlock::NONE;
      s.step = 0;
      s.struck = 0;
      s.step_done = 0;
      s.active = MatchBlock::NONE;
      s.auto_pick = false;
      s.auto_stage = MatchBlock::NONE;
    }
  }
  if (task && !s.done && two_players)
  {
    const std::vector<int> page0 = PageZero(m);
    StageSelect(m, rules, s, frame, page0);
  }
  else if (task && s.flow == 1)
  {
    // The opponent went (or the set ended): the stage select is the remaining player's again, and
    // the game's steps start over if they come back.
    s.flow = 0;
    s.step = 0;
    s.struck = 0;
    s.step_done = 0;
    s.active = MatchBlock::NONE;
    s.auto_pick = false;
    s.auto_stage = MatchBlock::NONE;
  }
}
}  // namespace

int LegalIndexOfKind(int kind)
{
  for (size_t i = 0; i < LEGAL_KINDS.size(); ++i)
  {
    if (LEGAL_KINDS[i] == kind)
      return static_cast<int>(i);
  }
  return -1;
}

std::vector<int> PageZero(const GuestMemory& m)
{
  std::vector<int> page;
  if (!m.Valid(RSS_EXDATA) || !m.Valid(RSS_EXDATA + RSS_EXDATA_SIZE - 1))
    return page;
  const int count = std::min<int>(m.Read8(RSS_PAGE0_COUNT), MAX_PAGE_STAGES);
  for (int pos = 0; pos < count; ++pos)
  {
    const u8 slot = m.Read8(RSS_PAGE0_SLOTS + pos);
    const u32 kind_at = RSS_SLOT_KINDS + 2u * slot;
    page.push_back(m.Valid(kind_at) ? LegalIndexOfKind(m.Read8(kind_at)) : -1);
  }
  return page;
}

int Apply(GuestMemory& m, int frame, bool two_players)
{
  const std::optional<MatchBlock::State> read = MatchBlock::Read(m);
  if (!read || !IsRankedPPlus(*read))
    return 0;
  MatchBlock::State s = *read;
  const std::string scene = ReadSceneName(m);
  const u32 task = StageSelectTask(m, scene);
  int changed = 0;
  // Outside the stage select, the 2024 Proposed preset: the stage select builds its pages from it.
  if (scene != "scSelStage" && m.Valid(RSS_EXDATA) && m.Valid(RSS_EXDATA + RSS_EXDATA_SIZE - 1))
  {
    for (u32 i = 0; i < RSS_EXDATA_SIZE; ++i)
    {
      if (m.Read8(RSS_EXDATA + i) != kSwitch03[i])
      {
        m.Write8(RSS_EXDATA + i, kSwitch03[i]);
        ++changed;
      }
    }
  }
  Step1(m, s, frame, two_players, scene, task);
  if (task && s.flow == 1 && !s.done && two_players)
  {
    const Ranked::Ruleset& rules = Ranked::PPlus2024();
    const Plan plan = Ranked::PlanGame(rules, ViewOf(s));
    const std::vector<int> page0 = PageZero(m);
    const int table = WriteStrikeTable(m, page0, ShownStruck(rules, plan, s));
    if (table > 0 && m.Valid(PAGE_INDEX))
    {
      // Project+'s buttonProc redraws the page's struck art when PAGE_INDEX isn't CURRENT_PAGE.
      if (m.Read8(PAGE_INDEX) != 0xFF)
      {
        m.Write8(PAGE_INDEX, 0xFF);
        ++changed;
      }
    }
    changed += table;
    const u32 controller = s.active <= 1 ? s.active : 0xF0;
    if (m.Read32(task + SSS_CONTROLLER) != controller)
    {
      m.Write32(task + SSS_CONTROLLER, controller);
      changed += 4;
    }
    if (s.auto_pick)
    {
      const int pos = PositionOf(page0, s.auto_stage);
      if (pos >= 0 && m.Read32(task + SSS_SELECTED) != static_cast<u32>(pos))
      {
        m.Write32(task + SSS_SELECTED, static_cast<u32>(pos));
        changed += 4;
      }
    }
  }
  changed += MatchBlock::Write(m, s);
  return changed;
}

Rollback::InputGate::Masks Masks(const GuestMemory& m)
{
  Rollback::InputGate::Masks masks{};
  const std::optional<MatchBlock::State> s = MatchBlock::Read(m);
  if (!s || !IsRankedPPlus(*s) || s->done || s->flow != 1 || s->active > 1)
    return masks;
  const std::string scene = ReadSceneName(m);
  const u32 task = StageSelectTask(m, scene);
  if (!task)
    return masks;
  const Plan plan = Ranked::PlanGame(Ranked::PPlus2024(), ViewOf(*s));
  if (s->step >= plan.steps.size())
    return masks;
  for (int port = 0; port < Rollback::InputGate::PORTS; ++port)
  {
    if (port != s->active)
      masks[port] = Rollback::InputGate::ALL;
  }
  Mask& mine = masks[s->active];
  if (s->auto_pick)
  {
    // The pick's timer ran out: the selection is the default, and A is pressed for the player.
    mine = Rollback::InputGate::ALL;
    mine.press = PAD_BUTTON_A;
    return masks;
  }
  mine.buttons = PAD_BUTTON_B | PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_TRIGGER_Z | PAD_BUTTON_START |
                 PAD_BUTTON_Y;
  const bool pick = plan.steps[s->step].kind == StepKind::Pick;
  mine.buttons |= pick ? PAD_BUTTON_X : PAD_BUTTON_A;
  // Random would strike every stage outside the random set, or pick for the player.
  if (m.Read32(task + SSS_HOVERED) == RANDOM_ITEM)
    mine.buttons |= PAD_BUTTON_A | PAD_BUTTON_X;
  return masks;
}

Lines Describe(const GuestMemory& m, int frame, const std::vector<Events::PortInfo>& ports)
{
  Lines lines;
  const std::optional<MatchBlock::State> s = MatchBlock::Read(m);
  if (!s || !IsRankedPPlus(*s))
    return lines;
  const std::string scene = ReadSceneName(m);
  if (scene == "scMelee")
    return lines;
  const auto name = [&](int port) {
    for (const Events::PortInfo& p : ports)
    {
      if (p.port == port && !p.name.empty())
        return p.name;
    }
    return fmt::format("P{}", port + 1);
  };
  const auto local = [&](int port) {
    for (const Events::PortInfo& p : ports)
    {
      if (p.port == port)
        return !p.remote;
    }
    return false;
  };
  const Ranked::Ruleset& rules = Ranked::PPlus2024();
  if (s->done && s->set_winner <= 1)
  {
    lines.set = fmt::format("Ranked · {} won the set {}–{}", name(s->set_winner),
                            std::max(s->score[0], s->score[1]), std::min(s->score[0], s->score[1]));
    return lines;
  }
  lines.set = fmt::format("Ranked · Game {} · {} {}–{} {}", s->games + 1, name(0), s->score[0],
                          s->score[1], name(1));
  if (scene != "scSelStage" || s->flow != 1 || s->active > 1)
    return lines;
  const Plan plan = Ranked::PlanGame(rules, ViewOf(*s));
  if (s->step >= plan.steps.size())
    return lines;
  const Step& step = plan.steps[s->step];
  const int left = std::max<int>(0, static_cast<int>(s->deadline) - rules.grace_frames - frame);
  const int seconds = (left + 59) / 60;
  const std::string clock = fmt::format("{}:{:02}", seconds / 60, seconds % 60);
  if (s->auto_pick && s->auto_stage < Ranked::MAX_STAGES)
  {
    lines.turn = fmt::format("Time's up · {}", rules.stage_names[s->auto_stage]);
    return lines;
  }
  const int owed = std::max(1, step.count - s->step_done);
  const bool mine = local(s->active);
  std::string what;
  switch (step.kind)
  {
  case StepKind::Strike:
    what = mine ? fmt::format("strike {} stage{} with X", owed, owed == 1 ? "" : "s") :
                  fmt::format("strikes {} stage{}", owed, owed == 1 ? "" : "s");
    break;
  case StepKind::Ban:
    what = mine ? fmt::format("ban {} stage{} with X", owed, owed == 1 ? "" : "s") :
                  fmt::format("bans {} stage{}", owed, owed == 1 ? "" : "s");
    break;
  case StepKind::Pick:
    what = mine ? std::string("pick the stage with A") : std::string("picks the stage");
    break;
  }
  lines.turn = mine ? fmt::format("Your turn: {} · {}", what, clock) :
                      fmt::format("{} {} · {}", name(s->active), what, clock);
  return lines;
}

// ---- Header ----
namespace
{
struct HeaderRequest
{
  u8 mode = MatchBlock::MODE_NONE;
  u8 coin = 0;
  std::string code;
};
std::mutex s_header_mutex;
std::optional<HeaderRequest> s_header_pending;
std::string s_hosted_code;

std::mutex s_lines_mutex;
Lines s_lines;

// The running game's ruleset: Project+ (its launcher on Brawl rev 2), else vanilla Brawl rev 2.
u8 RunningRuleset()
{
  const Orca::Profile* profile = Orca::ActiveProfile();
  if (!profile || !ResultsVerified())
    return Ranked::RULESET_NONE;
  return profile->IsLauncher() ? Ranked::RULESET_PPLUS_2024 : Ranked::RULESET_BRAWL_SUPERNOVA_2025;
}

// ORCA_TEST_RANKED=<coin 0|1> in a session (a test override): a ranked set from the first frame.
std::optional<u8> TestRanked()
{
  static const std::optional<u8> coin = []() -> std::optional<u8> {
    const std::string v = Orca::TestRankedSpec();
    if (v.empty())
      return std::nullopt;
    return static_cast<u8>(v == "1" ? 1 : 0);
  }();
  return coin;
}
}  // namespace

void RequestHeader(bool ranked, const std::string& code)
{
  std::lock_guard lock(s_header_mutex);
  s_header_pending = HeaderRequest{ranked ? MatchBlock::MODE_RANKED : MatchBlock::MODE_CASUAL,
                                   MatchBlock::CoinFromCode(code), code};
}

void ApplyHeader(GuestMemory& m, bool resimulating, bool alone)
{
  if (const std::optional<u8> coin = TestRanked())
  {
    // Every pass: the same header on every machine with the same override.
    const u8 ruleset = RunningRuleset();
    const std::optional<MatchBlock::State> s = MatchBlock::Read(m);
    if (ruleset != Ranked::RULESET_NONE &&
        (!s || s->mode != MatchBlock::MODE_RANKED || s->ruleset != ruleset || s->coin != *coin))
    {
      MatchBlock::Write(m, MatchBlock::State::Fresh(MatchBlock::MODE_RANKED, ruleset, *coin));
    }
    return;
  }
  if (resimulating || !alone)
    return;
  std::optional<HeaderRequest> request;
  {
    std::lock_guard lock(s_header_mutex);
    request = std::exchange(s_header_pending, std::nullopt);
    if (request)
      s_hosted_code = request->code;
  }
  if (request)
  {
    const u8 ruleset = RunningRuleset();
    MatchBlock::Write(m, MatchBlock::State::Fresh(request->mode, ruleset, request->coin));
    NOTICE_LOG_FMT(ROLLBACK, "Ranked: a new {} block, ruleset {}, game 1's first striker port {}",
                   request->mode == MatchBlock::MODE_RANKED ? "ranked" : "casual", ruleset,
                   request->coin + 1);
    return;
  }
  const std::optional<MatchBlock::State> s = MatchBlock::Read(m);
  if (!s)
    return;
  std::string hosted;
  {
    std::lock_guard lock(s_header_mutex);
    hosted = s_hosted_code;
  }
  // Still in the matchmade room it hosts, or playing in one: the set stays.
  const std::string code = Orca::Online::Code();
  if ((!hosted.empty() && code == hosted) || Orca::Online::RoomQueue() != "private")
    return;
  MatchBlock::Clear(m);
  {
    std::lock_guard lock(s_header_mutex);
    s_hosted_code.clear();
  }
  NOTICE_LOG_FMT(ROLLBACK, "Ranked: the match block cleared (no matchmade room)");
}

void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports, bool alone)
{
  if (!ResultsVerified())
    return;
  GuardMemory memory(guard);
  ApplyHeader(memory, resimulating, alone);
  bool p1 = false, p2 = false;
  for (const Events::PortInfo& p : ports)
  {
    p1 = p1 || p.port == 0;
    p2 = p2 || p.port == 1;
  }
  const std::optional<MatchBlock::State> before =
      resimulating ? std::nullopt : MatchBlock::Read(memory);
  Apply(memory, frame, p1 && p2);
  if (!resimulating)
  {
    // The flow's changes, first runs only (a re-run repeats them).
    const std::optional<MatchBlock::State> after = MatchBlock::Read(memory);
    if (after && IsRankedPPlus(*after) && (!before || *before != *after))
    {
      const MatchBlock::State& a = *after;
      const MatchBlock::State b = before.value_or(MatchBlock::State{});
      if (a.flow != b.flow || a.step != b.step || a.struck != b.struck ||
          a.auto_pick != b.auto_pick || a.games != b.games || a.fight != b.fight ||
          a.done != b.done)
      {
        NOTICE_LOG_FMT(
            ROLLBACK,
            "Ranked: frame {}: game {} score {}-{} flow {} step {} port {} struck {:03x} "
            "made {} auto {} fight {} stage {} won {:03x}/{:03x}{}",
            frame, a.games + 1, a.score[0], a.score[1], a.flow, a.step,
            a.active <= 1 ? a.active + 1 : 0, a.struck, a.step_done,
            a.auto_pick ? static_cast<int>(a.auto_stage) : -1, a.fight ? 1 : 0,
            a.stage == MatchBlock::NONE ? -1 : a.stage, a.won_on[0], a.won_on[1],
            a.done ? fmt::format(" SET port {}", a.set_winner + 1) : std::string());
      }
    }
    Lines lines = Describe(memory, frame, ports);
    std::lock_guard lock(s_lines_mutex);
    s_lines = std::move(lines);
  }
}

Lines CurrentLines()
{
  std::lock_guard lock(s_lines_mutex);
  return s_lines;
}
}  // namespace Orca::UX::RankedPPlus
