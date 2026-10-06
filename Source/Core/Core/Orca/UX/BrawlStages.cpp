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
#include "Core/Orca/UX/MatchBlock.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/OnlineMenu.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/UX/Results.h"
#include "Core/Orca/UX/SetBlock.h"
#include "Core/Orca/UX/StageCursors.h"

namespace Orca::UX::BrawlStages
{
static_assert(kBlock == FreeSpace::kMatchBlock.begin);
static_assert(kRegion >= FreeSpace::kMatchBlock.begin &&
              kRegion + kRegionSize <= FreeSpace::kMatchBlock.end);
static_assert(kResultsSet + 0x80 <= FreeSpace::kMatchBlockLimit);

namespace RS = Ranked;

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
  O_PREFS = 0x23,  // s8 x 2: a casual game's preferred stages
  O_SEED = 0x28,   // u32: a casual game's coin (State::seed)
  O_END = 0x2C,
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
  s.set.won_on = {r16(O_WON), r16(O_WON + 2)};
  s.coin = r8(O_COIN);
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
  s.progress.prefs = {static_cast<s8>(r8(O_PREFS)), static_cast<s8>(r8(O_PREFS + 1))};
  s.seed = m.Read32(kRegion + O_SEED);
  return s;
}

void WriteState(GuestMemory& m, const State& s)
{
  Put32(m, O_MAGIC, kMagic);
  Put8(m, O_VERSION, kVersion);
  Put8(m, O_GAME, static_cast<u8>(s.set.game));
  Put8(m, O_SCORE, s.score[0]);
  Put8(m, O_SCORE + 1, s.score[1]);
  Put8(m, O_LAST_WINNER, static_cast<u8>(static_cast<s8>(s.set.last_winner)));
  Put8(m, O_COIN, s.coin);
  Put16(m, O_WON, s.set.won_on[0]);
  Put16(m, O_WON + 2, s.set.won_on[1]);
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
  Put8(m, O_PREFS, static_cast<u8>(s.progress.prefs[0]));
  Put8(m, O_PREFS + 1, static_cast<u8>(s.progress.prefs[1]));
  Put32(m, O_SEED, s.seed);
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
constexpr std::array<Tile, kStages> kTiles{{
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
constexpr std::array<int, kStages> kStageKinds{
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

int StageAt(u32 page, s32 slot)
{
  for (int stage = 0; stage < kStages; ++stage)
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
  return RS::Brawl2025();
}

// `casual`: a casual game's two preferred-stage turns (RankedSteps.h PlanCasual) instead of the
// ranked set's steps.
RS::Plan PlanOf(const State& s, const RS::SetView& set, bool casual)
{
  RS::SetView v = set;
  v.coin = s.coin == 0xFF ? 0 : (s.coin & 1);
  v.characters = {s.characters[0], s.characters[1]};
  return casual ? RS::PlanCasual(Rules(), v, s.seed) : RS::PlanGame(Rules(), v);
}

// The port whose turn it is, or the last step's once the steps are done (the game's cursor is
// that port's while the flow makes the pick). A step that is both players' (a casual game's): port
// 1's.
int TurnPort(const RS::Plan& plan, const RS::Progress& progress)
{
  int port = 0;
  if (const RS::Step* step = RS::Current(plan, progress))
    port = step->port;
  else if (!plan.steps.empty())
    port = plan.steps.back().port;
  return port <= 1 ? port : 0;
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
  s->set.last_winner = w;
  if (const int stage = StageForStageId(r.block.stage); stage >= 0)
    s->set.won_on[w] = static_cast<u16>(s->set.won_on[w] | Bit(stage));
  s->set.game = static_cast<u8>(std::min(s->set.game + 1, 255));
}

// The game's cursor: written where the flow wants it (a write moves it; the hover follows the
// next frame).
void PutCursor(GuestMemory& m, const View& v, float x, float y)
{
  const auto cursor = Pointer(m, v.task + TASK_CURSOR);
  if (!cursor || !m.Valid(*cursor + CURSOR_Y))
    return;
  const u32 bx = std::bit_cast<u32>(x), by = std::bit_cast<u32>(y);
  if (m.Read32(*cursor + CURSOR_X) != bx)
    m.Write32(*cursor + CURSOR_X, bx);
  if (m.Read32(*cursor + CURSOR_Y) != by)
    m.Write32(*cursor + CURSOR_Y, by);
}

// A page turn the two cursors asked for (StageCursors::State::page_turn): bit 7 set, bit 0 the port
// that asked, bit 1 the page it was on.
constexpr u8 TURN_ON = 0x80;
constexpr u8 TURN_FRAMES = 90;  // gives up after this many of the screen's frames

// Each player's presses on their own cursor (StageCursors.h): X strikes or bans on the turn's
// player's own turn, Y skips an optional ban, A picks in the picker's pick step and otherwise
// proposes (a casual game: names the preferred stage), B takes a proposal back, A on the page
// button turns the page. Edges against what the hook saw, so a boundary run again sees none.
void CursorPresses(const RS::Plan& plan, State* s, StageCursors::State* c, u32 page)
{
  const StageCursors::Layout& layout = StageCursors::BrawlLayout();
  for (int port = 0; port < 2; ++port)
  {
    const u8 pressed = StageCursors::TakePresses(c, port);
    if (!pressed || s->progress.picked >= 0)
      continue;
    const StageCursors::Cursor& mine = c->cursors[port];
    const int on = StageCursors::Hovered(layout, page, mine.x, mine.y);
    const RS::Step* step = RS::Current(plan, s->progress);
    const bool my_turn = step && step->port == port;
    if ((pressed & StageCursors::BTN_A) && on == StageCursors::PAGE_BUTTON && c->page_turn == 0)
      c->page_turn = static_cast<u8>(TURN_ON | port | (page & 1) << 1);
    if (on >= 0 && my_turn && (pressed & StageCursors::BTN_X) &&
        (step->kind == RS::StepKind::Strike || step->kind == RS::StepKind::Ban))
    {
      RS::Strike(Rules(), plan, &s->progress, on);
    }
    else if (on >= 0 && (pressed & StageCursors::BTN_A))
    {
      // The picker's A picks (a stage DSR keeps from them is still theirs to propose).
      if (!(my_turn && step->kind == RS::StepKind::Pick && RS::Pick(plan, &s->progress, on)))
        RS::Propose(plan, &s->progress, port, on);
    }
    if (my_turn && step->optional && (pressed & StageCursors::BTN_Y) && s->progress.picked < 0)
      RS::Skip(Rules(), plan, &s->progress);
    if (pressed & StageCursors::BTN_B)
      RS::Withdraw(&s->progress, port);
  }
}

// The stage select's part of a boundary.
void SelectFrame(GuestMemory& m, const View& v, State* s, bool casual)
{
  const RS::SetView set = casual ? s->set : SetOf(m, *s);
  const StageCursors::Layout& layout = StageCursors::BrawlLayout();
  std::optional<StageCursors::State> cursors = StageCursors::Read(m);
  // A new stage select: this game's (its frame counter starts again on every visit, so steps left
  // from a set that ended mid-select never carry over).
  if (s->phase != Phase::Select || v.clock < s->clock)
  {
    // This game's stage select opens: its steps start.
    const auto characters = ReadCharacters(m);
    s->characters = characters ? *characters : std::array<s8, 2>{-1, -1};
    // Game 1's first striker: the header's coin (the room code's, the same on both machines).
    if (m.Valid(MatchBlock::COIN))
      s->coin = static_cast<u8>(m.Read8(MatchBlock::COIN) & 1);
    // A casual game's coin for its random choices: the room's, the header's coin, the game.
    s->seed = casual && m.Valid(MatchBlock::ROOM + 3) ?
                  RS::CasualSeed(m.Read32(MatchBlock::ROOM), s->coin, s->set.game) :
                  0;
    const RS::Plan plan = PlanOf(*s, set, casual);
    s->progress = RS::Begin(Rules(), plan);
    s->phase = Phase::Select;
    s->clock = v.clock;
    s->buttons = {};
    s->hovered = -1;
    s->hovered_for = 0;
    s->hover_key = 0xFF;
    cursors = StageCursors::Start(layout);
  }
  if (!cursors)
    cursors = StageCursors::Start(layout);
  StageCursors::State& c = *cursors;
  const RS::Plan plan = PlanOf(*s, set, casual);
  const int hovered = HoveredStage(m, v);
  const u8 key = HoverKey(m, v);
  const u32 state = m.Read32(v.task + TASK_STATE);
  const u32 page = m.Read32(v.task + TASK_PAGE);
  if (state != STATE_CHOOSING)
  {
    // The game took a stage. Only the flow presses A, and only on the stage it stands for, but a
    // press that still took another (the hover a frame behind the cursor) is replaced by the stage
    // the flow stands for (the player's rested stage in a pick, or what the steps' defaults
    // leave), before the screen copies it out.
    if (state == STATE_TAKEN)
    {
      if (const RS::Step* step = RS::Current(plan, s->progress))
      {
        const int taken = StageForStageId(static_cast<int>(m.Read32(v.task + TASK_TAKEN_KIND)));
        if (step->kind == RS::StepKind::Pick)
        {
          const u16 allowed = RS::Allowed(plan, s->progress);
          const int want = (taken >= 0 && (allowed & Bit(taken))) ? taken :
                           (s->hovered >= 0 && (allowed & Bit(s->hovered))) ?
                                                                    s->hovered :
                                                                    std::countr_zero(allowed);
          RS::Pick(plan, &s->progress, want);
        }
        else
        {
          for (int i = 0; i < 16 && s->progress.picked < 0 && RS::Current(plan, s->progress); ++i)
          {
            s->progress.frames = 1;
            RS::Tick(Rules(), plan, &s->progress);
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
    // Presses made meanwhile count for nothing.
    StageCursors::TakePresses(&c, 0);
    StageCursors::TakePresses(&c, 1);
    StageCursors::Write(m, c);
    s->clock = v.clock;
    return;
  }
  CursorPresses(plan, s, &c, page);
  // The turn timer runs on the stage select's own frames: once per frame, never twice at one
  // boundary. So does how long the game's cursor has rested where it is, and a page turn's wait.
  if (v.clock != s->clock)
  {
    s->clock = v.clock;
    RS::Tick(Rules(), plan, &s->progress);
    if (key == s->hover_key)
      s->hovered_for = static_cast<u8>(std::min(s->hovered_for + 1, 255));
    else
      s->hovered_for = 0;
    s->hover_key = key;
    s->hovered = static_cast<s8>(hovered);
    if (c.page_turn)
      c.page_frames = static_cast<u8>(std::min(c.page_frames + 1, 255));
  }
  // A page turn ends once the page is another (or after a while: the game never took it).
  if (c.page_turn &&
      ((page & 1) != static_cast<u32>((c.page_turn >> 1) & 1) || c.page_frames >= TURN_FRAMES ||
       s->progress.picked >= 0))
  {
    c.page_turn = 0;
    c.page_frames = 0;
  }
  if (!c.page_turn)
    c.page_frames = 0;

  // The game's cursor: on the stage the steps picked (through the page button first when it is on
  // the other page), on the page button while a page turn waits, else where the follower's cursor
  // is (its hover shows the stage's preview). The cursor's port is whose A the gate presses then.
  u32 port = c.follower;
  if (s->progress.picked >= 0)
  {
    const Tile& tile = kTiles[s->progress.picked];
    const bool other_page = page != tile.page;
    const StageCursors::Tile* t = StageCursors::TileOf(layout, s->progress.picked, tile.page);
    PutCursor(m, v, other_page ? PAGE_BUTTON_X : t ? StageCursors::MidX(t->rect) : tile.x,
              other_page ? PAGE_BUTTON_Y : t ? StageCursors::MidY(t->rect) : tile.y);
    port = static_cast<u32>(TurnPort(plan, s->progress));
  }
  else if (c.page_turn)
  {
    PutCursor(m, v, PAGE_BUTTON_X, PAGE_BUTTON_Y);
    port = c.page_turn & 1;
  }
  else
  {
    const StageCursors::Cursor& f = c.cursors[c.follower & 1];
    PutCursor(m, v, static_cast<float>(f.x) * (1.0f / 16.0f),
              static_cast<float>(f.y) * (1.0f / 16.0f));
  }
  if (m.Read32(v.task + TASK_PORT) != port)
    m.Write32(v.task + TASK_PORT, port);
  StageCursors::Write(m, c);

  // Grey tiles: every stage neither the current step nor an agreement can take.
  const u16 allowed = s->progress.picked >= 0 ?
                          Bit(s->progress.picked) :
                          static_cast<u16>(RS::Allowed(plan, s->progress) |
                                           RS::Proposable(plan, s->progress));
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
}
}  // namespace

int StageForStageId(int stage_id)
{
  for (int stage = 0; stage < kStages; ++stage)
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

constexpr std::array<const char*, kStages> kStageNames{
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
std::optional<bool> s_casual_for_tests;

bool HeaderSays(const GuestMemory& m, u8 mode)
{
  return m.Valid(kBlock + 7) && m.Read32(kBlock) == kHeaderMagic &&
         m.Read8(kBlock + 4) == kHeaderVersion && m.Read8(kBlock + 5) == mode &&
         m.Read8(kBlock + 6) == kRulesetBrawl;
}
}  // namespace

void SetRankedForTests(std::optional<bool> ranked)
{
  s_ranked_for_tests = ranked;
}

void SetCasualForTests(std::optional<bool> casual)
{
  s_casual_for_tests = casual;
}

bool RankedBrawlMatch(const GuestMemory& m)
{
  if (s_ranked_for_tests)
    return *s_ranked_for_tests;
  return HeaderSays(m, kModeRanked);
}

bool CasualBrawlMatch(const GuestMemory& m)
{
  if (s_casual_for_tests)
    return *s_casual_for_tests;
  return HeaderSays(m, kModeCasual);
}

RS::SetView SetOf(const GuestMemory& m, const State& s, std::array<u8, 2>* score)
{
  // The results track's set (SetBlock.h), the authoritative one, when the block holds a ranked
  // header and a set that reads sane.
  if (const auto header = SetBlock::ReadHeader(m); header && header->mode == SetBlock::Mode::Ranked)
  {
    const SetBlock::SetState b = SetBlock::ReadSet(m);
    if (b.games <= SetBlock::MAX_RECORDS && b.wins[0] + b.wins[1] <= b.games && b.done <= 2 &&
        (b.last_winner <= 1 || b.last_winner == SetBlock::NO_PORT))
    {
      RS::SetView set;
      set.game = b.GameNumber();
      set.last_winner = b.last_winner <= 1 ? b.last_winner : -1;
      for (int i = 0; i < b.games; ++i)
      {
        const SetBlock::GameRecord& r = b.records[i];
        const int stage = StageForStageId(r.stage);
        if (r.winner <= 1 && stage >= 0)
          set.won_on[r.winner] = static_cast<u16>(set.won_on[r.winner] | Bit(stage));
      }
      if (score)
        *score = b.wins;
      return set;
    }
  }
  if (score)
    *score = s.score;
  return s.set;
}

void Frame(GuestMemory& m, bool two_players)
{
  const std::optional<State> stored = ReadState(m);
  const bool ranked = RankedBrawlMatch(m);
  const bool casual = !ranked && two_players && CasualBrawlMatch(m);
  if (!ranked && !casual)
  {
    if (stored)
    {
      // Left mid-select (the opponent gone, the header cleared): the cursor is anyone's again.
      if (const View v = ReadView(m); v.present && stored->phase == Phase::Select &&
                                      m.Read32(v.task + TASK_PORT) != PORT_ANY)
      {
        m.Write32(v.task + TASK_PORT, PORT_ANY);
      }
      ClearState(m);
    }
    StageCursors::Clear(m);
    return;
  }
  State s = stored ? *stored : Fresh();
  const View v = ReadView(m);
  if (v.present)
  {
    SelectFrame(m, v, &s, casual);
  }
  else
  {
    // Off the stage select the two cursors stop (the latch moves nothing).
    StageCursors::Clear(m);
    const std::string scene = ReadSceneName(m);
    if (scene == "scMelee")
    {
      // A casual game counts its fights (the next stage select's coin is another).
      if (casual && s.phase == Phase::Select)
        s.set.game = static_cast<u8>(std::min(s.set.game + 1, 255));
      s.phase = Phase::Fight;
    }
    else if (scene == "scVsResult" && s.phase == Phase::Fight)
    {
      if (!casual)
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
  const bool casual = !RankedBrawlMatch(m) && CasualBrawlMatch(m);
  const RS::SetView set = casual ? s->set : SetOf(m, *s);
  const RS::Plan plan = PlanOf(*s, set, casual);
  // The two cursors (StageCursors.h) are the players' own: the game's cursor and its buttons are
  // nobody's. The flow acts through them.
  for (int port = 0; port < PORTS; ++port)
    (*masks)[port] = ALL;
  if (m.Read32(v.task + TASK_STATE) != STATE_CHOOSING || (v.clock & 1) != 0)
    return;
  const std::optional<StageCursors::State> c = StageCursors::Read(m);
  const bool rested = s->hovered_for >= 1 && s->hover_key == HoverKey(m, v);
  if (s->progress.picked >= 0)
  {
    // The steps (or the two players) picked: A on the stage (or on the page button first), every
    // other frame so each press is a new one, for the cursor's port.
    const int turn = TurnPort(plan, s->progress);
    const Tile& tile = kTiles[s->progress.picked];
    const bool on_page = m.Read32(v.task + TASK_PAGE) == tile.page;
    if ((on_page && s->hovered == s->progress.picked) || (!on_page && v.hovered_item == ITEM_PAGE))
      (*masks)[turn & 3].press |= PAD_BUTTON_A;
    return;
  }
  // A page turn: A on the page button once the game's cursor has rested there.
  if (c && c->page_turn && v.hovered_item == ITEM_PAGE && rested)
    (*masks)[c->page_turn & 1].press |= PAD_BUTTON_A;
}

std::optional<Ranked::Turn> CurrentTurn(const GuestMemory& m)
{
  // Lines' turn (the same state, plan and step), without the names.
  const std::optional<State> s = ReadState(m);
  if (!s || s->phase != Phase::Select || !ReadView(m).present)
    return std::nullopt;
  const bool casual = !RankedBrawlMatch(m) && CasualBrawlMatch(m);
  const RS::Plan plan = PlanOf(*s, casual ? s->set : SetOf(m, *s), casual);
  const RS::Step* step = RS::Current(plan, s->progress);
  if (!step)
    return std::nullopt;
  return Ranked::Turn{step->kind, step->port, std::max(1, step->count - s->progress.done)};
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
  const bool casual = !RankedBrawlMatch(m) && CasualBrawlMatch(m);
  std::array<u8, 2> wins{};
  const RS::SetView set = casual ? s->set : SetOf(m, *s, &wins);
  const auto local = [&](int port) {
    for (const Orca::Events::PortInfo& p : ports)
    {
      if (p.port == port)
        return !p.remote;
    }
    return false;
  };
  const auto who = [&](int port) -> std::string {
    for (const Orca::Events::PortInfo& p : ports)
    {
      if (p.port == port)
        return p.remote ? (p.name.empty() ? fmt::format("P{}", port + 1) : p.name) : "You";
    }
    return fmt::format("P{}", port + 1);
  };
  const auto name = [&](int port) -> std::string {
    for (const Orca::Events::PortInfo& p : ports)
    {
      if (p.port == port && !p.name.empty())
        return p.name;
    }
    return fmt::format("P{}", port + 1);
  };
  const RS::Plan plan = PlanOf(*s, set, casual);
  const RS::Step* step =
      v.present && s->phase == Phase::Select ? RS::Current(plan, s->progress) : nullptr;
  // The local player's port (-1: watching).
  int me = -1;
  for (const Orca::Events::PortInfo& p : ports)
  {
    if (!p.remote && (p.port == 0 || p.port == 1))
      me = p.port;
  }
  // What the other player proposes, for the local player's line ("bo wants Smashville").
  const auto proposal_note = [&]() -> std::string {
    const auto& prefs = s->progress.prefs;
    std::string out;
    for (int port = 0; port < 2; ++port)
    {
      if (port == me || prefs[port] < 0 || prefs[port] >= kStages)
        continue;
      out = fmt::format("{} wants {}", name(port), kStageNames[prefs[port]]);
      if (me >= 0 && prefs[me] != prefs[port])
        out += " · A on it to agree";
    }
    return out;
  };
  const auto join = [](std::string a, const std::string& b) {
    if (a.empty())
      return b;
    if (!b.empty())
      a += " · " + b;
    return a;
  };
  if (casual)
  {
    // Casual: only the stage select's pick and the stage it comes to.
    if (!v.present || s->phase != Phase::Select)
      return {};
    if (step && step->kind == RS::StepKind::Prefer)
    {
      const int seconds = RS::SecondsShown(Rules(), *step, s->progress.frames);
      std::string note;
      if (me >= 0 && s->progress.prefs[me] >= 0)
        note = fmt::format("You want {}", kStageNames[s->progress.prefs[me]]);
      else if (me >= 0)
        note = "A on the stage you want · the same pick plays there, else a coin takes one of "
               "the two";
      note = join(note, proposal_note());
      return {fmt::format("Stage pick · 0:{:02}", seconds), note};
    }
    if (s->progress.picked < 0)
      return {};
    const auto& prefs = s->progress.prefs;
    const int chosen = RS::CasualChoice(plan);
    const std::string whose = prefs[0] == prefs[1]    ? "both picks" :
                              local(chosen)           ? "your pick" :
                                                        fmt::format("{}'s pick", name(chosen));
    return {fmt::format("{} ({})", kStageNames[s->progress.picked], whose), {}};
  }
  // Ranked: the set on the character and stage selects; on the stage select, after it, whose turn
  // it is, how many stages they strike or ban, and the time left; under it the turn's buttons for
  // the player whose turn it is, A's proposal for both, and what the other player proposes.
  // No "Game N" (Luis, 2026-10-05: the dots say which game it is).
  const std::string score = fmt::format("{} {}–{} {}", who(0), wins[0], wins[1], who(1));
  if (!v.present || s->phase != Phase::Select)
    return {score, {}};
  if (step)
  {
    const std::string player = who(step->port);
    const bool you = local(step->port);
    const int left = step->count - s->progress.done;
    const std::string clock =
        fmt::format("0:{:02}", RS::SecondsShown(Rules(), *step, s->progress.frames));
    std::string turn, keys;
    switch (step->kind)
    {
    case RS::StepKind::Strike:
      turn = fmt::format("{} {} {}", player, you ? "strike" : "strikes", left);
      keys = "X on a stage strikes it";
      break;
    case RS::StepKind::Ban:
      turn = fmt::format("{} may ban {}", player, left);
      keys = "X on a stage bans it · Y skips";
      break;
    case RS::StepKind::Pick:
      turn = fmt::format("{}{} {}{}", plan.mk_clause ? "MK clause: " : "", player,
                         you ? "pick" : "picks", plan.mk_clause ? " any stage" : "");
      keys = "A on a stage picks it";
      break;
    case RS::StepKind::Prefer:
      break;
    }
    // Under it: the turn's keys for its player, then what the other player proposes, or else that
    // A proposes a stage.
    std::string note;
    if (you)
      note = keys;
    const std::string theirs = proposal_note();
    if (!theirs.empty())
      note = join(note, theirs);
    else if (me >= 0 && !(you && step->kind == RS::StepKind::Pick))
      note = join(note, "A proposes a stage");
    return {fmt::format("{} · {} · {}", score, turn, clock), note};
  }
  if (s->progress.picked >= 0)
  {
    const auto& prefs = s->progress.prefs;
    const bool agreed = prefs[0] >= 0 && prefs[0] == prefs[1] && prefs[0] == s->progress.picked;
    return {fmt::format("{} · {}: {}", score, agreed ? "Agreed" : "Stage",
                        kStageNames[s->progress.picked]),
            {}};
  }
  return {score, {}};
}

StageCursors::View CursorView(const GuestMemory& m,
                              const std::vector<Orca::Events::PortInfo>& ports)
{
  StageCursors::View view;
  const std::optional<State> s = ReadState(m);
  const std::optional<StageCursors::State> c = StageCursors::Read(m);
  const View v = ReadView(m);
  if (!s || !c || !v.present || s->phase != Phase::Select)
    return view;
  const bool casual = !RankedBrawlMatch(m) && CasualBrawlMatch(m);
  const RS::Plan plan = PlanOf(*s, casual ? s->set : SetOf(m, *s), casual);
  view.on = true;
  view.game = StageCursors::GAME_BRAWL;
  view.page = m.Read32(v.task + TASK_PAGE);
  view.picked = s->progress.picked;
  view.mk_clause = plan.mk_clause;
  if (const RS::Step* step = RS::Current(plan, s->progress))
  {
    if (step->port <= 1)
      view.turn = static_cast<s8>(step->port);
    // The step for the overlay's turn cues (UI only).
    view.kind = static_cast<u8>(step->kind);
    view.left = static_cast<u8>(std::max(1, step->count - s->progress.done));
    view.optional = step->optional;
    view.both = step->port == RS::BOTH;
    view.seconds = static_cast<s16>(RS::SecondsShown(Rules(), *step, s->progress.frames));
  }
  for (int port = 0; port < 2; ++port)
  {
    StageCursors::View::Player& p = view.players[port];
    p.x = c->cursors[port].x;
    p.y = c->cursors[port].y;
    p.proposal = s->progress.prefs[port];
    p.name = fmt::format("P{}", port + 1);
    for (const Orca::Events::PortInfo& info : ports)
    {
      if (info.port != port)
        continue;
      p.present = true;
      p.local = !info.remote;
      if (!info.name.empty())
        p.name = info.name;
    }
  }
  return view;
}

void FrameHook(const Core::CPUThreadGuard& guard, bool resimulating,
               const std::vector<Orca::Events::PortInfo>& ports)
{
  if (!VanillaBrawl())
    return;
  GuardMemory memory(guard);
  bool p1 = false, p2 = false;
  for (const Orca::Events::PortInfo& p : ports)
  {
    p1 = p1 || p.port == 0;
    p2 = p2 || p.port == 1;
  }
  const std::optional<State> before = resimulating ? std::nullopt : ReadState(memory);
  Frame(memory, p1 && p2);
  if (!resimulating)
  {
    // The flow's changes, first runs only (a re-run repeats them).
    const std::optional<State> after = ReadState(memory);
    if (after && (!before || before->phase != after->phase ||
                  before->progress.step != after->progress.step ||
                  before->progress.struck != after->progress.struck ||
                  before->progress.picked != after->progress.picked ||
                  before->progress.prefs != after->progress.prefs))
    {
      NOTICE_LOG_FMT(ROLLBACK,
                     "Brawl {} stages: game {} phase {} step {} struck {:03x} prefs {}/{} picked {} "
                     "coin {} score {}-{}",
                     RankedBrawlMatch(memory) ? "ranked" : "casual", after->set.game,
                     static_cast<int>(after->phase), after->progress.step, after->progress.struck,
                     after->progress.prefs[0], after->progress.prefs[1], after->progress.picked,
                     after->coin, after->score[0], after->score[1]);
    }
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
