// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/OnlineRules.h"

#include <atomic>
#include <map>
#include <mutex>
#include <utility>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/OnlineMenu.h"
#include "Core/Orca/UX/Probe.h"
#include "InputCommon/GCPadStatus.h"

namespace Orca::UX::Rules
{
namespace
{
namespace B = MatchBlock;

constexpr u32 GAME_GLOBAL = 0x805A00E0;
constexpr u32 GLOBAL_MODE_MELEE = 0x08;
constexpr u32 GLOBAL_SET_RULE = 0x1C;
// gmGlobalModeMelee: the players' init data, 4 x 0x5C from +0x98 (+0x00 character, +0x01 type:
// 0 human, 1 CPU, 3 none).
constexpr u32 MELEE_PLAYERS = 0x98;
constexpr u32 MELEE_PLAYER_SIZE = 0x5C;
constexpr u8 PLAYER_HUMAN = 0;

// gmSetRule (BrawlHeaders gm_set_rule.h, the research's probes): +0x02 the rule in its low 3 bits
// (0 time, 1 stock), +0x04 stocks, +0x05 handicap, +0x06 damage ratio x10, +0x07 stage choice
// (0 Choose, 1 Random), +0x08 a stock match's time limit in minutes, +0x09 team attack, +0x0A
// pause.
constexpr u32 RULE_MODE = 0x02;
constexpr u32 RULE_STOCKS = 0x04;
constexpr u32 RULE_HANDICAP = 0x05;
constexpr u32 RULE_RATIO = 0x06;
constexpr u32 RULE_STAGE_CHOICE = 0x07;
constexpr u32 RULE_STOCK_MINUTES = 0x08;
constexpr u32 RULE_TEAM_ATTACK = 0x09;
constexpr u32 RULE_PAUSE = 0x0A;
constexpr u32 RULE_SAVED_FROM = 0x02;
constexpr u32 RULE_SAVED_SIZE = 10;  // +0x02..+0x0B
constexpr u8 STAGE_CHOOSE = 0;
constexpr u8 STAGE_RANDOM = 1;

// gmGlobalRecord's menu data (0x9017B640 + 0x810, both games; RSBE01.patches "Rules"): +0x00 the
// item frequency (u8, 0 None), +0x08 the item switch (64 bits), +0x20 the Random Stage Switch's
// Melee page, +0x24 its Brawl page.
constexpr u32 MENU_DATA = 0x9017BE50;
constexpr u32 MENU_ITEM_FREQUENCY = MENU_DATA + 0x00;
constexpr u32 MENU_ITEM_SWITCH_HI = MENU_DATA + 0x08;
constexpr u32 MENU_ITEM_SWITCH_LO = MENU_DATA + 0x0C;
constexpr u32 MENU_STAGES_MELEE = MENU_DATA + 0x20;
constexpr u32 MENU_STAGES_BRAWL = MENU_DATA + 0x24;

// The scene manager (OnlineMenu.cpp): +0x04 the current scene. The character select's scene:
// +0x400 its task, whose +0x44 + 4 x port are the players' panels: +0x1B4 the panel's kind,
// +0x1B8 its character (the research's probes; ReadCss).
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 SCENE_CURRENT = 0x04;
constexpr u32 CSS_TASK = 0x400;
constexpr u32 CSS_PANELS = 0x44;
constexpr u32 PANEL_KIND = 0x1B4;
constexpr u32 PANEL_CHARACTER = 0x1B8;

// Project+'s stage switch data (Net-Random.asm RSS_EXDATA, its preset loaded from the SD card) and
// its strike table (STAGE_STRIKE_TABLE: 6 bytes per page, 5 pages).
constexpr u32 PPLUS_RSS = 0x8042C4E8;
constexpr u32 PPLUS_STRIKES = 0x8042C822;
constexpr u32 PPLUS_STRIKES_SIZE = 0x1E;

// The 2024 Proposed stage list, as Project+'s own preset 3 (pf/stage/switch/Switch03.rss) holds it,
// over the bytes where it differs from the netplay build's default preset: the pages' random
// masks, the hazard switches (all off), and the three pages' layouts (page 0: Battlefield,
// Pokemon Stadium 2, Smashville, Luigi's Mansion, Temple of Time, Green Hill Zone, Bowser's
// Castle, Frigate Husk, Dream Land; every one of them in the random set).
constexpr std::array<u8, B::SAVED_RSS_SIZE> PPLUS_2024_RSS{
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
    0x3A, 0x2C, 0x1B, 0x1E, 0x30, 0x2B, 0x00, 0x00,
};

constexpr const char* SCENE_CSS = "scSelctCharacter";
constexpr const char* SCENE_SSS = "scSelStage";
constexpr const char* SCENE_FIGHT = "scMelee";
constexpr const char* SCENE_BETWEEN = "scMemoryChange";

// Phase 1's own flags in the block: +0xC3 bit 0, a fight began since the header was written (the
// set's first game is under way or done: the ready timer's no-show applies only before it).
constexpr u32 FOUGHT = B::BASE + 0xC3;

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

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

// Writes only what differs, counting the bytes changed.
struct Writer
{
  GuestMemory& m;
  int changed = 0;
  void U8(u32 a, u8 v)
  {
    if (!m.Valid(a) || m.Read8(a) == v)
      return;
    m.Write8(a, v);
    ++changed;
  }
  void U16(u32 a, u16 v)
  {
    U8(a, static_cast<u8>(v >> 8));
    U8(a + 1, static_cast<u8>(v));
  }
  void U32(u32 a, u32 v)
  {
    U16(a, static_cast<u16>(v >> 16));
    U16(a + 2, static_cast<u16>(v));
  }
  void Zero(u32 a, u32 n)
  {
    for (u32 i = 0; i < n; ++i)
      U8(a + i, 0);
  }
};

bool BlockMapped(const GuestMemory& m)
{
  return m.Valid(B::BASE) && m.Valid(B::BASE + B::SIZE - 1);
}

u8 StocksFor(Ruleset ruleset)
{
  return ruleset == Ruleset::PPlus ? 4 : 3;
}

u32 CssTask(const GuestMemory& m)
{
  if (!Pointer(m, SCENE_MANAGER))
    return 0;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + SCENE_CURRENT))
    return 0;
  const u32 scene = m.Read32(manager + SCENE_CURRENT);
  if (!Pointer(m, scene + CSS_TASK))
    return 0;
  const u32 task = m.Read32(scene + CSS_TASK);
  return Pointer(m, task + CSS_PANELS + 12) ? task : 0;
}

// The players' characters for the match about to start, from its init data (filled as the
// character select ends: the stage select and the scenes between).
std::array<CssPort, 4> ReadMatchPlayers(const GuestMemory& m)
{
  std::array<CssPort, 4> out{};
  const u32 melee = GlobalField(m, GLOBAL_MODE_MELEE);
  if (!melee || !m.Valid(melee + MELEE_PLAYERS + 4 * MELEE_PLAYER_SIZE))
    return out;
  for (u32 i = 0; i < 4; ++i)
  {
    const u32 p = melee + MELEE_PLAYERS + i * MELEE_PLAYER_SIZE;
    out[i].human = m.Read8(p + 1) == PLAYER_HUMAN;
    if (out[i].human)
      out[i].character = m.Read8(p);
  }
  return out;
}

bool AnyIceClimbers(const std::array<CssPort, 4>& ports)
{
  for (const CssPort& p : ports)
  {
    if (p.human && p.character == CHARACTER_ICE_CLIMBERS)
      return true;
  }
  return false;
}

void ForceRules(Writer& w, const Header& h)
{
  const u32 rule = GlobalField(w.m, GLOBAL_SET_RULE);
  if (rule && w.m.Valid(rule + RULE_PAUSE))
  {
    w.U8(rule + RULE_MODE, static_cast<u8>((w.m.Read8(rule + RULE_MODE) & 0xF8) | 1));
    w.U8(rule + RULE_STOCKS, StocksFor(h.ruleset));
    w.U8(rule + RULE_HANDICAP, 0);
    w.U8(rule + RULE_RATIO, 10);
    w.U8(rule + RULE_STAGE_CHOICE, h.mode == Mode::Casual ? STAGE_RANDOM : STAGE_CHOOSE);
    w.U8(rule + RULE_STOCK_MINUTES, 8);
    w.U8(rule + RULE_TEAM_ATTACK, 1);
    w.U8(rule + RULE_PAUSE, 0);
  }
  // Items: none, and every item off.
  w.U8(MENU_ITEM_FREQUENCY, 0);
  w.U32(MENU_ITEM_SWITCH_HI, 0);
  w.U32(MENU_ITEM_SWITCH_LO, 0);
}

void ForceStages(Writer& w, const Header& h, const std::string& scene)
{
  if (h.ruleset == Ruleset::Brawl)
  {
    // The characters as they stand: on the character select its panels, from its end on the
    // match's init data (a Random character is resolved there). Elsewhere the list stays as it is.
    std::array<CssPort, 4> ports;
    if (scene == SCENE_CSS)
      ports = ReadCss(w.m);
    else if (scene == SCENE_SSS || scene == SCENE_BETWEEN)
      ports = ReadMatchPlayers(w.m);
    else
      return;
    const bool ice = AnyIceClimbers(ports);
    w.U32(MENU_STAGES_BRAWL, BRAWL_LEGAL_BRAWL_PAGE & ~(ice ? BRAWL_FINAL_DESTINATION_BIT : 0));
    w.U32(MENU_STAGES_MELEE, BRAWL_LEGAL_MELEE_PAGE);
    return;
  }
  if (h.ruleset == Ruleset::PPlus && scene == SCENE_CSS)
  {
    // Before the stage select builds its pages from it (and before Random picks from it).
    for (u32 i = 0; i < B::SAVED_RSS_SIZE; ++i)
      w.U8(PPLUS_RSS + i, PPLUS_2024_RSS[i]);
    // Casual's Random never shows the stage select, so nothing clears strikes left from before.
    if (h.mode == Mode::Casual)
      w.Zero(PPLUS_STRIKES, PPLUS_STRIKES_SIZE);
  }
}

// Ranked's character select: the no-ready timer (MatchBlock CSS_TIMER, CSS_NO_SHOW).
void ReadyTimer(Writer& w, const Header& h, const std::string& scene)
{
  if (scene == SCENE_FIGHT)
    w.U8(FOUGHT, static_cast<u8>(w.m.Read8(FOUGHT) | 1));
  if (scene != SCENE_CSS)
  {
    w.U16(B::CSS_TIMER, 0);
    w.U8(B::CSS_NO_SHOW, 0);
    return;
  }
  if (h.mode != Mode::Ranked || (w.m.Read8(FOUGHT) & 1) || w.m.Read8(B::CSS_NO_SHOW) != 0)
    return;
  const std::array<CssPort, 4> ports = ReadCss(w.m);
  int ready = 0, waiting = -1;
  for (int i = 0; i < 4; ++i)
  {
    if (!ports[i].human)
      continue;
    if (ports[i].character >= 0)
      ++ready;
    else if (waiting < 0)
      waiting = i;
  }
  if (ready == 0 || waiting < 0)
  {
    w.U16(B::CSS_TIMER, 0);
    return;
  }
  const int t = w.m.Read16(B::CSS_TIMER) + 1;
  if (t >= CSS_READY_FRAMES)
  {
    w.U16(B::CSS_TIMER, 0);
    w.U8(B::CSS_NO_SHOW, static_cast<u8>(waiting + 1));
    return;
  }
  w.U16(B::CSS_TIMER, static_cast<u16>(t));
}

// ---- Session state (CPU thread) ----
std::atomic<Mode> s_wanted{Mode::None};
std::atomic<bool> s_settled{true};
std::mutex s_mutex;
std::optional<Mode> s_expect;
std::optional<std::string> s_mismatch;

// The no-show byte per frame, until its frame is final (as Results.h's tracker).
struct NoShowTracker
{
  std::map<int, u8> pending;
  u64 resyncs = 0;
  bool started = false;
  u8 last = 0;
  int last_frame = -1;
};
NoShowTracker& NoShows()
{
  static NoShowTracker t;
  return t;
}
}  // namespace

Header ReadHeader(const GuestMemory& m)
{
  Header h;
  if (!BlockMapped(m) || m.Read32(B::MAGIC) != B::MAGIC_VALUE ||
      m.Read8(B::VERSION) != B::VERSION_VALUE)
  {
    return h;
  }
  h.present = true;
  const u8 mode = m.Read8(B::MODE);
  const u8 ruleset = m.Read8(B::RULESET);
  h.mode = mode <= 2 ? static_cast<Mode>(mode) : Mode::None;
  h.ruleset = ruleset <= 2 ? static_cast<Ruleset>(ruleset) : Ruleset::None;
  return h;
}

Ruleset ProfileRuleset()
{
  const Orca::Profile* profile = Orca::ActiveProfile();
  if (!profile || profile->revision != 2)
    return Ruleset::None;
  if (profile->IsLauncher())
    return profile->game_id == "PPLUS32" && profile->disc == "RSBE01" ? Ruleset::PPlus :
                                                                        Ruleset::None;
  return profile->game_id == "RSBE01" ? Ruleset::Brawl : Ruleset::None;
}

std::array<CssPort, 4> ReadCss(const GuestMemory& m)
{
  std::array<CssPort, 4> out{};
  const u32 task = CssTask(m);
  if (!task)
    return out;
  for (u32 i = 0; i < 4; ++i)
  {
    const u32 panel = m.Read32(task + CSS_PANELS + 4 * i);
    if (!Pointer(m, panel + PANEL_CHARACTER))
      continue;
    // The panel's kind: 0 a human player's, 1 a CPU's, 2 closed (the research's probes).
    const u32 kind = m.Read32(panel + PANEL_KIND);
    out[i].human = kind == 0;
    if (!out[i].human)
      continue;
    const u32 character = m.Read32(panel + PANEL_CHARACTER);
    out[i].character = character < 0x40 ? static_cast<int>(character) : -1;
  }
  return out;
}

int WriteHeader(GuestMemory& m, Mode mode, Ruleset ruleset)
{
  if (!BlockMapped(m))
    return 0;
  Writer w{m};
  const Header now = ReadHeader(m);
  if (mode == Mode::None || ruleset == Ruleset::None)
  {
    // Nothing of Orca's there (the game's own dead code, or zeros already): never touched.
    if (!now.present)
      return 0;
    const u8 flags = m.Read8(B::SAVED_FLAGS);
    if (flags & 1)
    {
      if (const u32 rule = GlobalField(m, GLOBAL_SET_RULE); rule && m.Valid(rule + RULE_PAUSE))
      {
        for (u32 i = 0; i < RULE_SAVED_SIZE; ++i)
          w.U8(rule + RULE_SAVED_FROM + i, m.Read8(B::SAVED_RULES + i));
      }
      w.U32(MENU_ITEM_FREQUENCY, m.Read32(B::SAVED_ITEMS));
      w.U32(MENU_ITEM_SWITCH_HI, m.Read32(B::SAVED_ITEMS + 4));
      w.U32(MENU_ITEM_SWITCH_LO, m.Read32(B::SAVED_ITEMS + 8));
      w.U32(MENU_STAGES_MELEE, m.Read32(B::SAVED_STAGES));
      w.U32(MENU_STAGES_BRAWL, m.Read32(B::SAVED_STAGES + 4));
    }
    if ((flags & 2) && m.Valid(PPLUS_RSS + B::SAVED_RSS_SIZE - 1))
    {
      for (u32 i = 0; i < B::SAVED_RSS_SIZE; ++i)
        w.U8(PPLUS_RSS + i, m.Read8(B::SAVED_RSS + i));
    }
    w.Zero(B::BASE, B::SIZE);
    return w.changed;
  }
  if (now.present && now.mode == mode && now.ruleset == ruleset)
    return 0;
  if (!now.present)
  {
    // From the game's dead code (or a cleared block): all zeros, then the player's own rules.
    w.Zero(B::BASE, B::SIZE);
    u8 flags = 0;
    if (const u32 rule = GlobalField(m, GLOBAL_SET_RULE); rule && m.Valid(rule + RULE_PAUSE))
    {
      for (u32 i = 0; i < RULE_SAVED_SIZE; ++i)
        w.U8(B::SAVED_RULES + i, m.Read8(rule + RULE_SAVED_FROM + i));
      if (m.Valid(MENU_STAGES_BRAWL))
      {
        w.U32(B::SAVED_ITEMS, m.Read32(MENU_ITEM_FREQUENCY));
        w.U32(B::SAVED_ITEMS + 4, m.Read32(MENU_ITEM_SWITCH_HI));
        w.U32(B::SAVED_ITEMS + 8, m.Read32(MENU_ITEM_SWITCH_LO));
        w.U32(B::SAVED_STAGES, m.Read32(MENU_STAGES_MELEE));
        w.U32(B::SAVED_STAGES + 4, m.Read32(MENU_STAGES_BRAWL));
        flags |= 1;
      }
    }
    if (ruleset == Ruleset::PPlus && m.Valid(PPLUS_RSS + B::SAVED_RSS_SIZE - 1))
    {
      for (u32 i = 0; i < B::SAVED_RSS_SIZE; ++i)
        w.U8(B::SAVED_RSS + i, m.Read8(PPLUS_RSS + i));
      flags |= 2;
    }
    w.U8(B::SAVED_FLAGS, flags);
  }
  else
  {
    // Another queue room's header (the saved rules stay the player's own): a new set.
    w.Zero(B::SET, B::SET_SIZE);
    w.Zero(B::STEP, B::STEP_SIZE);
    w.Zero(B::CSS_TIMER, B::PHASE1_SIZE);
  }
  w.U32(B::MAGIC, B::MAGIC_VALUE);
  w.U8(B::VERSION, B::VERSION_VALUE);
  w.U8(B::MODE, static_cast<u8>(mode));
  w.U8(B::RULESET, static_cast<u8>(ruleset));
  return w.changed;
}

int ApplyLocks(GuestMemory& m)
{
  const Header h = ReadHeader(m);
  if (!h.Locked())
    return 0;
  Writer w{m};
  const std::string scene = ReadSceneName(m);
  ForceRules(w, h);
  ForceStages(w, h, scene);
  ReadyTimer(w, h, scene);
  return w.changed;
}

Rollback::InputGate::Masks GateMasks(const GuestMemory& m)
{
  Rollback::InputGate::Masks masks{};
  const Header h = ReadHeader(m);
  if (!h.Locked())
    return masks;
  const std::string scene = ReadSceneName(m);
  u16 buttons = 0;
  if (scene == SCENE_CSS)
  {
    // No B: holding it is the way back to the menus (and B is never needed to pick).
    buttons = PAD_BUTTON_B;
    // Project+'s Code Menu opens from the character select with the D-pad.
    if (h.ruleset == Ruleset::PPlus)
      buttons |= PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT;
  }
  else if (scene == SCENE_SSS)
  {
    // No way back to the character select once the stage is being picked.
    buttons = PAD_BUTTON_B;
    // Project+: L/R alternate stages, Z hazards, L+R+A back to the character select.
    if (h.ruleset == Ruleset::PPlus)
      buttons |= PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_TRIGGER_Z;
  }
  else if (scene == SCENE_FIGHT)
  {
    // Pause off (Brawl's rule says so; Project+ pauses on a held Start whatever the rule).
    buttons = PAD_BUTTON_START;
  }
  for (Rollback::InputGate::Mask& mask : masks)
    mask.buttons = buttons;
  return masks;
}

void SetWanted(Mode mode)
{
  if (s_wanted.exchange(mode) != mode)
    s_settled = false;
}

Mode Wanted()
{
  return s_wanted;
}

bool HeaderSettled()
{
  return s_settled;
}

void ExpectHeader(Mode mode)
{
  std::lock_guard lock(s_mutex);
  s_expect = mode;
}

std::optional<std::string> TakeMismatch()
{
  std::lock_guard lock(s_mutex);
  return std::exchange(s_mismatch, std::nullopt);
}

std::optional<Mode> TestQueueMode()
{
  static const std::optional<Mode> mode = []() -> std::optional<Mode> {
    const std::string v = Orca::TestQueueSpec();
    if (v == "casual")
      return Mode::Casual;
    if (v == "ranked")
      return Mode::Ranked;
    if (!v.empty())
      ERROR_LOG_FMT(ROLLBACK, "Online rules: ORCA_TEST_QUEUE={}: not casual or ranked", v);
    return std::nullopt;
  }();
  return TestKnobsAllowed() ? mode : std::nullopt;
}

std::optional<int> ConfirmNoShow(int confirmed)
{
  NoShowTracker& t = NoShows();
  std::optional<int> out;
  for (auto it = t.pending.begin(); it != t.pending.end() && it->first <= confirmed;
       it = t.pending.erase(it))
  {
    if (it->second != 0 && t.last == 0 && !out)
      out = it->second - 1;
    t.last = it->second;
    t.last_frame = it->first;
  }
  return out;
}

void OnFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating, bool alone)
{
  const Ruleset ruleset = ProfileRuleset();
  if (ruleset == Ruleset::None)
  {
    s_settled = true;
    return;
  }
  GuardMemory m(guard);
  if (alone && !resimulating)
  {
    Mode wanted = s_wanted;
    if (const auto test = TestQueueMode())
      wanted = *test;
    if (const int changed = WriteHeader(m, wanted, ruleset); changed > 0)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Online rules: header {} at frame {} ({} bytes)",
                     wanted == Mode::Casual ? "casual" :
                     wanted == Mode::Ranked ? "ranked" :
                                              "cleared",
                     frame, changed);
    }
    s_settled = true;
  }
  const int locked = ApplyLocks(m);
  if (locked > 0 && !resimulating)
    DEBUG_LOG_FMT(ROLLBACK, "Online rules: {} bytes forced at frame {}", locked, frame);
  // A joiner's check, once its keyframe is in.
  if (!resimulating)
  {
    std::lock_guard lock(s_mutex);
    if (s_expect)
    {
      const Header h = ReadHeader(m);
      const Mode expected = *s_expect;
      s_expect.reset();
      const bool ok = expected == Mode::None ?
                          !h.Locked() :
                          h.Locked() && h.mode == expected && h.ruleset == ruleset;
      if (!ok)
      {
        WARN_LOG_FMT(ROLLBACK,
                     "Online rules: the host's game says mode {} ruleset {}, this room wants "
                     "mode {} ruleset {}: mismatch",
                     static_cast<int>(h.mode), static_cast<int>(h.ruleset),
                     static_cast<int>(expected), static_cast<int>(ruleset));
        s_mismatch = "mismatch";
      }
      else
      {
        NOTICE_LOG_FMT(ROLLBACK, "Online rules: the host's header matches this room (mode {})",
                       static_cast<int>(expected));
      }
    }
  }
  // The no-show byte for this frame (a re-run's replaces the first run's), until it is final.
  NoShowTracker& t = NoShows();
  const u64 resyncs = Orca::Events::Resyncs();
  if (!t.started || t.resyncs != resyncs)
  {
    t = {};
    t.started = true;
    t.resyncs = resyncs;
  }
  if (frame > t.last_frame)
  {
    t.pending[frame] = ReadHeader(m).Locked() ? m.Read8(B::CSS_NO_SHOW) : 0;
    while (t.pending.size() > 4096)
      t.pending.erase(t.pending.begin());
  }
}

void Install()
{
  Rollback::InputGate::SetSource([](const Core::CPUThreadGuard& guard) {
    if (ProfileRuleset() == Ruleset::None)
      return Rollback::InputGate::Masks{};
    GuardMemory m(guard);
    return GateMasks(m);
  });
}

void Uninstall()
{
  Rollback::InputGate::SetSource({});
}
}  // namespace Orca::UX::Rules
