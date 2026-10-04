// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/BrawlStages.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/FreeSpace.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/OnlineMenu.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/UX/Results.h"

namespace Orca::UX::BrawlStages
{
static_assert(kBlock == FreeSpace::kMatchBlock.begin);
static_assert(kRegion >= FreeSpace::kMatchBlock.begin &&
              kRegion + kRegionSize <= FreeSpace::kMatchBlock.end);
static_assert(kResultsSet + 0x80 <= FreeSpace::kMatchBlockLimit);

namespace RS = RankedSteps;

namespace
{
// The region's layout (big-endian, as the game's memory).
enum Offset : u32
{
  O_MAGIC = 0x00,    // u32
  O_VERSION = 0x04,  // u8
  O_GAME = 0x05,
  O_SCORE = 0x06,  // u8 x 2
  O_LAST_WINNER = 0x08,
  O_COIN = 0x09,
  O_WON = 0x0A,  // u16 x 2
  O_PHASE = 0x0E,
  O_STEP = 0x0F,
  O_DONE = 0x10,
  O_PICKED = 0x11,
  O_STRUCK = 0x12,      // u16
  O_FRAMES = 0x14,      // u16
  O_CHARACTERS = 0x16,  // s8 x 2
  O_BUTTONS = 0x18,     // u16 x 2
  O_CLOCK = 0x1C,       // u32
  O_HOVERED = 0x20,
  O_HOVERED_FOR = 0x21,
  O_HOVER_KEY = 0x22,
  O_END = 0x23,
};
static_assert(O_END <= kRegionSize);

void Put8(GuestMemory& m, u32 offset, u8 value)
{
  if (m.Read8(kRegion + offset) != value)
    m.Write8(kRegion + offset, value);
}
void Put16(GuestMemory& m, u32 offset, u16 value)
{
  Put8(m, offset, static_cast<u8>(value >> 8));
  Put8(m, offset + 1, static_cast<u8>(value));
}
void Put32(GuestMemory& m, u32 offset, u32 value)
{
  Put16(m, offset, static_cast<u16>(value >> 16));
  Put16(m, offset + 2, static_cast<u16>(value));
}

// A pointer the game holds, if it points into RAM.
std::optional<u32> Pointer(const GuestMemory& m, u32 address)
{
  if (!m.Valid(address))
    return std::nullopt;
  const u32 p = m.Read32(address);
  if (!m.Valid(p))
    return std::nullopt;
  return p;
}
}  // namespace

std::optional<State> ReadState(const GuestMemory& m)
{
  if (!m.Valid(kRegion) || m.Read32(kRegion + O_MAGIC) != kMagic ||
      m.Read8(kRegion + O_VERSION) != kVersion)
  {
    return std::nullopt;
  }
  State s;
  const auto r8 = [&](u32 o) { return m.Read8(kRegion + o); };
  const auto r16 = [&](u32 o) { return m.Read16(kRegion + o); };
  s.set.game = r8(O_GAME);
  s.score = {r8(O_SCORE), r8(O_SCORE + 1)};
  s.set.last_winner = static_cast<s8>(r8(O_LAST_WINNER));
  s.coin = r8(O_COIN);
  s.set.won = {r16(O_WON), r16(O_WON + 2)};
  s.phase = static_cast<Phase>(r8(O_PHASE));
  s.progress.step = r8(O_STEP);
  s.progress.done = r8(O_DONE);
  s.progress.picked = static_cast<s8>(r8(O_PICKED));
  s.progress.struck = r16(O_STRUCK);
  s.progress.frames = r16(O_FRAMES);
  s.characters = {static_cast<s8>(r8(O_CHARACTERS)), static_cast<s8>(r8(O_CHARACTERS + 1))};
  s.buttons = {r16(O_BUTTONS), r16(O_BUTTONS + 2)};
  s.clock = m.Read32(kRegion + O_CLOCK);
  s.hovered = static_cast<s8>(r8(O_HOVERED));
  s.hovered_for = r8(O_HOVERED_FOR);
  s.hover_key = r8(O_HOVER_KEY);
  return s;
}

void WriteState(GuestMemory& m, const State& s)
{
  Put32(m, O_MAGIC, kMagic);
  Put8(m, O_VERSION, kVersion);
  Put8(m, O_GAME, s.set.game);
  Put8(m, O_SCORE, s.score[0]);
  Put8(m, O_SCORE + 1, s.score[1]);
  Put8(m, O_LAST_WINNER, static_cast<u8>(s.set.last_winner));
  Put8(m, O_COIN, s.coin);
  Put16(m, O_WON, s.set.won[0]);
  Put16(m, O_WON + 2, s.set.won[1]);
  Put8(m, O_PHASE, static_cast<u8>(s.phase));
  Put8(m, O_STEP, s.progress.step);
  Put8(m, O_DONE, s.progress.done);
  Put8(m, O_PICKED, static_cast<u8>(s.progress.picked));
  Put16(m, O_STRUCK, s.progress.struck);
  Put16(m, O_FRAMES, s.progress.frames);
  Put8(m, O_CHARACTERS, static_cast<u8>(s.characters[0]));
  Put8(m, O_CHARACTERS + 1, static_cast<u8>(s.characters[1]));
  Put16(m, O_BUTTONS, s.buttons[0]);
  Put16(m, O_BUTTONS + 2, s.buttons[1]);
  Put32(m, O_CLOCK, s.clock);
  Put8(m, O_HOVERED, static_cast<u8>(s.hovered));
  Put8(m, O_HOVERED_FOR, s.hovered_for);
  Put8(m, O_HOVER_KEY, s.hover_key);
}

void ClearState(GuestMemory& m)
{
  // Only the magic: a reader treats anything else as no set.
  if (m.Valid(kRegion) && m.Read32(kRegion + O_MAGIC) == kMagic)
    Put32(m, O_MAGIC, 0);
}
}  // namespace Orca::UX::BrawlStages

namespace Orca::UX::BrawlStages
{
namespace
{
// ---- The stage select's layout (Brawl rev 2, an empty save; from the probes) ----
// The task: +0x200 the cursor (+0x3C x, +0x40 y, floats), +0x224 the select state (0 choosing),
// +0x228 the page (0 Brawl's stages, 1 Melee's), +0x250 frames
// since the screen opened, +0x615C the stage kind it took.
constexpr u32 TASK_CURSOR = 0x200;
constexpr u32 CURSOR_X = 0x3C;
constexpr u32 CURSOR_Y = 0x40;
constexpr u32 TASK_STATE = 0x224;
constexpr u32 STATE_CHOOSING = 0;
constexpr u32 STATE_TAKEN = 2;  // A took a stage: the screen leaves about 6 frames later
// The stage A took (srStageKind), set with STATE_TAKEN and copied out as the screen leaves: a write
// in between changes the stage the match loads.
constexpr u32 TASK_TAKEN_KIND = 0x258;
constexpr u32 TASK_PAGE = 0x228;
// MuStageTblAccess's table (sora_menu_sel_stage): per page {the page's stage ids, u8 count}.
constexpr u32 PAGE_TABLE = 0x806B9298;
constexpr u32 TASK_CLOCK = 0x250;
// The hovered item's special ids.
constexpr u32 ITEM_PAGE = 0x35;
constexpr u32 ITEM_RANDOM = 0x36;
constexpr u32 ITEM_BACK = 0x37;
// An icon (task +0x8C + 4 x slot) -> +0x14 -> +0x10 -> +0x18: the picture's frame (float);
// 0.0 is a plain grey tile.
constexpr u32 ICON_A = 0x14;
constexpr u32 ICON_B = 0x10;
constexpr u32 ICON_FRAME = 0x18;
constexpr u32 GREY = 0x00000000;  // 0.0f

// Brawl rev 2's stage select (probes, empty save): page 0 holds Brawl's 31 stages in the order of
// MuStageTblAccess's table (0x806B9298: per page a list of the select's stage ids, 0x806B99D8 for
// page 0, 0x806B9A58 for page 1), page 1 Melee's 10. The cursor runs over x -30..30, y -19.5..19.5
// (up is +y); icons are about 6.2 wide. The points are inside each icon where the probes' cursor
// sweeps hovered it.
constexpr std::array<Tile, RS::BRAWL_STAGES> kTiles{{
    {0, 0, -1.5f, 15.74f},   // Battlefield (the select's stage id 0x00)
    {1, 9, 22.0f, -8.7f},    // Pokemon Stadium (0x28), the Melee page's bottom right
    {0, 13, 11.2f, 4.46f},   // Lylat Cruise (0x0E)
    {0, 20, 23.0f, -3.06f},  // Smashville (0x1A)
    {0, 11, -1.6f, 4.46f},   // Yoshi's Island (0x0C)
    {0, 1, 4.7f, 15.74f},    // Final Destination (0x01)
    {0, 2, 11.3f, 15.74f},   // Delfino Plaza (0x02)
}};
// The page button (Melee Stages / Brawl Stages), bottom right on both pages.
constexpr float PAGE_BUTTON_X = 23.0f;
constexpr float PAGE_BUTTON_Y = -18.11f;
}  // namespace

const Tile& TileFor(int stage)
{
  return kTiles[static_cast<size_t>(stage) % kTiles.size()];
}

namespace
{

// The legal stages' stage kinds (srStageKind), in RankedSteps' order.
constexpr std::array<int, RS::BRAWL_STAGES> kStageKinds{
    0x01,  // Battlefield
    0x2E,  // Pokemon Stadium (Melee)
    0x13,  // Lylat Cruise
    0x21,  // Smashville
    0x0D,  // Yoshi's Island (Brawl)
    0x02,  // Final Destination
    0x03,  // Delfino Plaza
};

// gmGlobalModeMelee (g_GameGlobal +0x08): +0x98 the players, 4 x 0x5C, +0x00 the character,
// +0x01 the player type (0 human). TBD(probe): the character select fills them before the stage
// select.
constexpr u32 GLOBAL_MODE_MELEE = 0x08;
constexpr u32 MELEE_PLAYERS = 0x98;
constexpr u32 MELEE_PLAYER_SIZE = 0x5C;
constexpr u8 TYPE_HUMAN = 0;

u16 Bit(int stage)
{
  return static_cast<u16>(1u << stage);
}

// A field of a struct g_GameGlobal points to.
std::optional<u32> GlobalField(const GuestMemory& m, u32 field)
{
  const auto global = Pointer(m, GAME_GLOBAL);
  if (!global)
    return std::nullopt;
  return Pointer(m, *global + field);
}

// Ports 1 and 2's characters for the coming game, if both are human.
std::optional<std::array<s8, 2>> ReadCharacters(const GuestMemory& m)
{
  const auto melee = GlobalField(m, GLOBAL_MODE_MELEE);
  if (!melee || !m.Valid(*melee + MELEE_PLAYERS + 2 * MELEE_PLAYER_SIZE))
    return std::nullopt;
  std::array<s8, 2> out{};
  for (u32 port = 0; port < 2; ++port)
  {
    const u32 p = *melee + MELEE_PLAYERS + port * MELEE_PLAYER_SIZE;
    if (m.Read8(p + 1) != TYPE_HUMAN)
      return std::nullopt;
    out[port] = static_cast<s8>(m.Read8(p));
  }
  return out;
}

// The buttons the game read from a port in the last frame (gfPadButtons).
u16 PadButtons(const GuestMemory& m, int port)
{
  const auto pads = Pointer(m, PAD_SYSTEM);
  if (!pads)
    return 0;
  const u32 a = *pads + PAD_SYSTEM_PADS + static_cast<u32>(port) * PAD_STRIDE;
  return m.Valid(a) ? static_cast<u16>(m.Read32(a)) : u16(0);
}

int StageAt(u32 page, s32 slot)
{
  for (int stage = 0; stage < RS::BRAWL_STAGES; ++stage)
  {
    if (kTiles[stage].page == page && kTiles[stage].slot == slot)
      return stage;
  }
  return -1;
}

int HoveredStage(const GuestMemory& m, const View& v)
{
  // +0x248 keeps the last slot after the cursor leaves the icons: only a stage item (not 0, the
  // page button, Random or Back) means the cursor is on that slot.
  if (!v.present || v.hovered_item == 0 || v.hovered_item == ITEM_PAGE ||
      v.hovered_item == ITEM_RANDOM || v.hovered_item == ITEM_BACK || v.selected < 0)
  {
    return -1;
  }
  return StageAt(m.Read32(v.task + TASK_PAGE), v.selected);
}

// What the cursor is on, as one byte: the page and the hovered item (0xFF nothing).
u8 HoverKey(const GuestMemory& m, const View& v)
{
  if (!v.present || v.hovered_item == 0 || v.hovered_item > 0x3F)
    return 0xFF;
  return static_cast<u8>((m.Read32(v.task + TASK_PAGE) & 3) << 6 | v.hovered_item);
}

const RS::Ruleset& Rules()
{
  return RS::BrawlSupernova2025();
}

RS::Plan PlanOf(const State& s, const RS::SetView& set)
{
  return RS::PlanGame(Rules(), set, s.coin == 0xFF ? 0 : s.coin,
                      {s.characters[0], s.characters[1]});
}

// The port whose turn it is, or the last step's once the steps are done (it drives the cursor
// while the steps make the pick).
int TurnPort(const RS::Plan& plan, const RS::Progress& progress)
{
  if (const RS::Step* step = RS::Current(plan, progress))
    return step->port;
  return plan.count > 0 ? plan.steps[plan.count - 1].port : 0;
}

State Fresh()
{
  State s;
  s.set.game = 1;
  s.set.last_winner = -1;
  return s;
}

// The result screen's verdict into the set.
void Record(const GuestMemory& m, State* s)
{
  const Reading r = ReadResults(m);
  if (r.scene != Reading::Scene::Results)
    return;
  const GameResult g = Judge(r.block, 0);
  if (g.kind != GameResult::Kind::Win || g.winner_port < 0 || g.winner_port > 1)
    return;  // a draw or a void game is played again (RankedSteps: the same steps)
  const int w = g.winner_port;
  s->score[w] = static_cast<u8>(std::min(s->score[w] + 1, 255));
  s->set.last_winner = static_cast<s8>(w);
  if (const int stage = StageForStageId(r.block.stage); stage >= 0)
    s->set.won[w] = static_cast<u16>(s->set.won[w] | Bit(stage));
  s->set.game = static_cast<u8>(std::min(s->set.game + 1, 255));
}

// The stage select's part of a boundary.
void SelectFrame(GuestMemory& m, const View& v, State* s)
{
  const RS::SetView set = SetOf(m, *s);
  // A new stage select: this game's (its frame counter starts again on every visit, so steps left
  // from a set that ended mid-select never carry over).
  if (s->phase != Phase::Select || v.clock < s->clock)
  {
    // This game's stage select opens: its steps start.
    const auto characters = ReadCharacters(m);
    s->characters = characters ? *characters : std::array<s8, 2>{-1, -1};
    if (s->coin == 0xFF && m.Valid(RNG_SEED))
      s->coin = RS::CoinFromSeed(m.Read32(RNG_SEED));
    const RS::Plan plan = PlanOf(*s, set);
    s->progress = RS::Begin(plan);
    RS::AutoPick(Rules(), plan, &s->progress, set);
    s->phase = Phase::Select;
    s->clock = v.clock;
    s->buttons = {PadButtons(m, 0), PadButtons(m, 1)};
    s->hovered = -1;
    s->hovered_for = 0;
    s->hover_key = 0xFF;
  }
  const RS::Plan plan = PlanOf(*s, set);
  const int hovered = HoveredStage(m, v);
  const u8 key = HoverKey(m, v);
  const u32 state = m.Read32(v.task + TASK_STATE);
  if (state != STATE_CHOOSING)
  {
    // The game took a stage. The gate leaves A only where the steps allow it, but a press in the
    // frame the cursor crosses onto another icon can still take that one: the stage the flow
    // stands for replaces it (the player's rested stage in a pick, or what the steps' defaults
    // leave), before the screen copies it out.
    if (state == STATE_TAKEN)
    {
      if (const RS::Step* step = RS::Current(plan, s->progress))
      {
        const int taken = StageForStageId(static_cast<int>(m.Read32(v.task + TASK_TAKEN_KIND)));
        if (step->kind == RS::Kind::Pick)
        {
          const u16 allowed = RS::Allowed(Rules(), plan, s->progress, set);
          const int want = (taken >= 0 && (allowed & Bit(taken))) ? taken :
                           (s->hovered >= 0 && (allowed & Bit(s->hovered))) ?
                                                                    s->hovered :
                                                                    std::countr_zero(allowed);
          RS::Pick(Rules(), plan, &s->progress, set, want);
        }
        else
        {
          for (int i = 0; i < 16 && s->progress.picked < 0 && RS::Current(plan, s->progress); ++i)
          {
            s->progress.frames = 1;
            RS::Tick(Rules(), plan, &s->progress, set);
          }
        }
      }
      if (s->progress.picked >= 0)
      {
        const u32 kind = static_cast<u32>(kStageKinds[s->progress.picked]);
        if (m.Read32(v.task + TASK_TAKEN_KIND) != kind)
          m.Write32(v.task + TASK_TAKEN_KIND, kind);
      }
    }
    s->buttons = {PadButtons(m, 0), PadButtons(m, 1)};
    s->clock = v.clock;
    return;
  }
  // The active port's presses since the last boundary (edges against what the block kept, so a
  // boundary run again after a load sees none).
  if (const RS::Step* step = RS::Current(plan, s->progress))
  {
    const int port = step->port;
    const u16 now = PadButtons(m, port);
    const u16 pressed = static_cast<u16>(now & ~s->buttons[port]);
    if ((step->kind == RS::Kind::Strike || step->kind == RS::Kind::Ban) && (pressed & GF_X) &&
        hovered >= 0)
    {
      RS::Strike(Rules(), plan, &s->progress, set, hovered);
    }
    else if (step->optional && (pressed & GF_Y))
    {
      RS::Skip(plan, &s->progress);
      RS::AutoPick(Rules(), plan, &s->progress, set);
    }
  }
  // The turn timer runs on the stage select's own frames: once per frame, never twice at one
  // boundary.
  // So does how long the cursor has rested on a stage.
  if (v.clock != s->clock)
  {
    s->clock = v.clock;
    RS::Tick(Rules(), plan, &s->progress, set);
    if (key == s->hover_key)
      s->hovered_for = static_cast<u8>(std::min(s->hovered_for + 1, 255));
    else
      s->hovered_for = 0;
    s->hover_key = key;
    s->hovered = static_cast<s8>(hovered);
  }
  s->buttons = {PadButtons(m, 0), PadButtons(m, 1)};

  // The screen: whose turn, the grey tiles, and the cursor while the steps make the pick.
  const u32 port = static_cast<u32>(TurnPort(plan, s->progress));
  if (m.Read32(v.task + TASK_PORT) != port)
    m.Write32(v.task + TASK_PORT, port);
  const u16 allowed = s->progress.picked >= 0 ? Bit(s->progress.picked) :
                                                RS::Allowed(Rules(), plan, s->progress, set);
  const u32 page = m.Read32(v.task + TASK_PAGE);
  // The page's icons (MuStageTblAccess: 31 and 10; the task's own count stays 31).
  const u32 table = PAGE_TABLE + 8 * std::min<u32>(page, 2) + 4;
  const u32 count = std::min<u32>(m.Valid(table) ? m.Read8(table) : 0, 40);
  for (u32 slot = 0; slot < count; ++slot)
  {
    const int stage = StageAt(page, static_cast<s32>(slot));
    if (stage >= 0 && (allowed & Bit(stage)))
      continue;
    const auto icon = Pointer(m, v.task + TASK_ICONS + 4 * slot);
    if (!icon)
      continue;
    const auto a = Pointer(m, *icon + ICON_A);
    if (!a)
      continue;
    const auto b = Pointer(m, *a + ICON_B);
    if (!b || !m.Valid(*b + ICON_FRAME))
      continue;
    if (m.Read32(*b + ICON_FRAME) != GREY)
      m.Write32(*b + ICON_FRAME, GREY);
  }
  // The steps picked (the last starter, or a timer's default): the cursor goes there (through the
  // page button first when the stage is on the other page), and Masks presses A on it.
  if (s->progress.picked >= 0)
  {
    const Tile& tile = kTiles[s->progress.picked];
    const bool other_page = page != tile.page;
    const float x = other_page ? PAGE_BUTTON_X : tile.x;
    const float y = other_page ? PAGE_BUTTON_Y : tile.y;
    if (const auto cursor = Pointer(m, v.task + TASK_CURSOR); cursor && m.Valid(*cursor + CURSOR_Y))
    {
      m.Write32(*cursor + CURSOR_X, std::bit_cast<u32>(x));
      m.Write32(*cursor + CURSOR_Y, std::bit_cast<u32>(y));
    }
  }
}
}  // namespace

int StageForStageId(int stage_id)
{
  for (int stage = 0; stage < RS::BRAWL_STAGES; ++stage)
  {
    if (kStageKinds[stage] == stage_id)
      return stage;
  }
  return -1;
}

View ReadView(const GuestMemory& m)
{
  View v;
  if (ReadSceneName(m) != "scSelStage")
    return v;
  const auto manager = Pointer(m, SCENE_MANAGER);
  if (!manager)
    return v;
  const auto scene = Pointer(m, *manager + 4);
  if (!scene)
    return v;
  const auto task = Pointer(m, *scene + SCENE_SSS_TASK);
  if (!task || !m.Valid(*task + TASK_PORT) || !m.Valid(*task + TASK_CLOCK))
    return v;
  v.present = true;
  v.task = *task;
  v.hovered_item = m.Read32(*task + TASK_HOVER);
  v.selected = static_cast<s32>(m.Read32(*task + TASK_SELECTED));
  v.clock = m.Read32(*task + TASK_CLOCK);
  return v;
}
}  // namespace Orca::UX::BrawlStages

namespace Orca::UX::BrawlStages
{
namespace
{

constexpr std::array<const char*, RS::BRAWL_STAGES> kStageNames{
    "Battlefield",    "Pokemon Stadium",   "Lylat Cruise",  "Smashville",
    "Yoshi's Island", "Final Destination", "Delfino Plaza",
};

bool VanillaBrawl()
{
  const Orca::Profile* profile = Orca::ActiveProfile();
  return profile && !profile->IsLauncher() && profile->game_id == "RSBE01" &&
         profile->revision == 2;
}
}  // namespace

namespace
{
std::optional<bool> s_ranked_for_tests;
}

void SetRankedForTests(std::optional<bool> ranked)
{
  s_ranked_for_tests = ranked;
}

bool RankedBrawlMatch(const GuestMemory& m)
{
  if (s_ranked_for_tests)
    return *s_ranked_for_tests;
  // The test knob: the same in both processes of a test.
  static const bool knob = Orca::TestRankedSpec() == "brawl";
  if (knob)
    return true;
  return m.Valid(kBlock + 7) && m.Read32(kBlock) == kHeaderMagic &&
         m.Read8(kBlock + 4) == kHeaderVersion && m.Read8(kBlock + 5) == kModeRanked &&
         m.Read8(kBlock + 6) == kRulesetBrawl;
}

RS::SetView SetOf(const GuestMemory& m, const State& s, std::array<u8, 2>* score)
{
  constexpr int MAX_RECORDS = 8;
  constexpr u32 RECORDS = 0x20, RECORD_SIZE = 0x0C;
  if (m.Valid(kBlock) && m.Read32(kBlock) == kHeaderMagic &&
      m.Valid(kResultsSet + RECORDS + RECORD_SIZE * MAX_RECORDS))
  {
    const u8 games = m.Read8(kResultsSet), w0 = m.Read8(kResultsSet + 1),
             w1 = m.Read8(kResultsSet + 2), done = m.Read8(kResultsSet + 3),
             last = m.Read8(kResultsSet + 5);
    if (games <= MAX_RECORDS && w0 + w1 <= games && done <= 2 &&
        (last == 0 || last == 1 || last == 0xFF))
    {
      RS::SetView set;
      set.game = static_cast<u8>(w0 + w1 + 1);
      set.last_winner = last == 0xFF ? s8(-1) : static_cast<s8>(last);
      for (u32 i = 0; i < games; ++i)
      {
        const u32 record = kResultsSet + RECORDS + RECORD_SIZE * i;
        const u8 winner = m.Read8(record + 4);
        const int stage = StageForStageId(m.Read8(record + 6));
        if (winner <= 1 && stage >= 0)
          set.won[winner] = static_cast<u16>(set.won[winner] | Bit(stage));
      }
      if (score)
        *score = {w0, w1};
      return set;
    }
  }
  if (score)
    *score = s.score;
  return s.set;
}

void Frame(GuestMemory& m)
{
  const std::optional<State> stored = ReadState(m);
  if (!RankedBrawlMatch(m))
  {
    if (stored)
      ClearState(m);
    return;
  }
  State s = stored ? *stored : Fresh();
  const View v = ReadView(m);
  if (v.present)
  {
    SelectFrame(m, v, &s);
  }
  else
  {
    const std::string scene = ReadSceneName(m);
    if (scene == "scMelee")
    {
      s.phase = Phase::Fight;
    }
    else if (scene == "scVsResult" && s.phase == Phase::Fight)
    {
      Record(m, &s);
      s.phase = Phase::Recorded;
    }
  }
  WriteState(m, s);
}

void Masks(const GuestMemory& m, Rollback::InputGate::Masks* masks)
{
  const std::optional<State> s = ReadState(m);
  if (!s || s->phase != Phase::Select)
    return;
  const View v = ReadView(m);
  if (!v.present)
    return;
  using namespace Rollback::InputGate;
  const RS::SetView set = SetOf(m, *s);
  const RS::Plan plan = PlanOf(*s, set);
  const RS::Step* step = RS::Current(plan, s->progress);
  const int turn = TurnPort(plan, s->progress);
  const auto add = [&](int port, u16 buttons, bool sticks) {
    Mask& mask = (*masks)[port];
    mask.buttons |= buttons;
    mask.main_stick = mask.main_stick || sticks;
    mask.c_stick = mask.c_stick || sticks;
  };
  // Everybody but the turn's port does nothing on this screen; nobody once the steps pick.
  for (int port = 0; port < PORTS; ++port)
  {
    if (!step || port != turn)
      add(port, ALL_BUTTONS, true);
  }
  if (m.Read32(v.task + TASK_STATE) != STATE_CHOOSING)
  {
    // A stage is taken: nobody does anything until the screen leaves.
    add(turn, ALL_BUTTONS, true);
    return;
  }
  if (step)
  {
    // Never back to the character select, and nothing but the cursor, X, Y and A.
    u16 off = PAD_BUTTON_B | PAD_BUTTON_START | PAD_TRIGGER_Z | PAD_TRIGGER_L | PAD_TRIGGER_R;
    // A turns the page, and in a pick takes an allowed stage, once the cursor has rested there a
    // frame.
    bool a = false;
    if (s->hovered_for >= 1 && s->hover_key == HoverKey(m, v))
    {
      a = v.hovered_item == ITEM_PAGE;
      if (step->kind == RS::Kind::Pick && s->hovered >= 0)
        a = a || (RS::Allowed(Rules(), plan, s->progress, set) & Bit(s->hovered));
    }
    if (!a)
      off |= PAD_BUTTON_A;
    add(turn, off, false);
    return;
  }
  // The steps picked: A on the stage (or on the page button first), every other frame so each
  // press is a new one.
  if (s->progress.picked < 0 || (v.clock & 1) != 0)
    return;
  const Tile& tile = kTiles[s->progress.picked];
  const bool on_page = m.Read32(v.task + TASK_PAGE) == tile.page;
  if ((on_page && s->hovered == s->progress.picked) || (!on_page && v.hovered_item == ITEM_PAGE))
    (*masks)[turn].press |= PAD_BUTTON_A;
}

std::pair<std::string, std::string> Lines(const GuestMemory& m,
                                          const std::vector<Orca::Events::PortInfo>& ports)
{
  const std::optional<State> s = ReadState(m);
  if (!s)
    return {};
  const View v = ReadView(m);
  if (!v.present && ReadSceneName(m) != "scSelctCharacter")
    return {};
  std::array<u8, 2> wins{};
  const RS::SetView set = SetOf(m, *s, &wins);
  const auto who = [&](int port) -> std::string {
    for (const Orca::Events::PortInfo& p : ports)
    {
      if (p.port == port)
        return p.remote ? (p.name.empty() ? fmt::format("P{}", port + 1) : p.name) : "You";
    }
    return fmt::format("P{}", port + 1);
  };
  std::string score =
      fmt::format("Game {} · {} {}–{} {}", set.game, who(0), wins[0], wins[1], who(1));
  if (!v.present || s->phase != Phase::Select)
    return {score, {}};
  const RS::Plan plan = PlanOf(*s, set);
  const RS::Step* step = RS::Current(plan, s->progress);
  std::string turn;
  if (step)
  {
    const std::string name = who(step->port);
    const bool you = name == "You";
    const int left = step->count - s->progress.done;
    switch (step->kind)
    {
    case RS::Kind::Strike:
      turn = fmt::format("{} {} {} (X)", name, you ? "strike" : "strikes", left);
      break;
    case RS::Kind::Ban:
      turn = fmt::format("{} may ban a stage (X) or skip (Y)", name);
      break;
    case RS::Kind::Pick:
      turn = fmt::format("{}{} {} the stage (A)", plan.mk_clause ? "MK clause: " : "", name,
                         you ? "pick" : "picks");
      break;
    default:
      break;
    }
    turn += fmt::format(" · 0:{:02}", RS::SecondsShown(s->progress.frames));
  }
  else if (s->progress.picked >= 0)
  {
    turn = fmt::format("Stage: {}", kStageNames[s->progress.picked]);
  }
  return {score, turn};
}

void FrameHook(const Core::CPUThreadGuard& guard, bool resimulating,
               const std::vector<Orca::Events::PortInfo>& ports)
{
  if (!VanillaBrawl())
    return;
  GuardMemory memory(guard);
  Frame(memory);
  if (!resimulating)
  {
    auto [line, note] = Lines(memory, ports);
    SetRulesLines(std::move(line), std::move(note));
  }
}

void GateMasks(const Core::CPUThreadGuard& guard, Rollback::InputGate::Masks* masks)
{
  if (!VanillaBrawl())
    return;
  GuardMemory memory(guard);
  Masks(memory, masks);
}
}  // namespace Orca::UX::BrawlStages
