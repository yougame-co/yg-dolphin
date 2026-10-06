// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/MatchBlock.h"
#include "InputCommon/GCPadStatus.h"

// Two cursors on a queue match's stage select, one per player (ORCA.md "The stage select's two
// cursors"; Luis, 2026-10-04: "it'd also be really helpful if each player had their own cursor in
// stage select"). The game's stage select has one cursor, driven by one port; both games' stage
// flows (BrawlStages.h, RankedPPlus.h) take every port's pad away from it while the two cursors run
// and act for the players through it (the cursor moved where a stage is taken, the gate's
// synthesized A), so the game only ever sees what the flows decide.
//
// Each player's cursor is synced state in the match block: its position in fixed point (1/16 of the
// stage select's own cursor units, integer math only), moved by that player's own stick, and the
// buttons of each frame. The input gate's latch (InputGate.h SetLatch) writes them from ports 1-2's
// raw pads of the frame about to run, as a pure function of memory and those pads, on every pass
// over a boundary (first runs, re-runs, a joiner's first frame after its keyframe), so the next
// boundary's frame hook reads the same cursors on every machine. The frame hook takes each port's
// presses as edges against what it saw (also in the block), so a boundary run again changes
// nothing. Which tile a cursor is on comes from the tile rectangles below (probed in each game, in
// the cursor's own units). The overlay draws the cursors and the players' proposals from what the
// frame hook publishes (UI only, first runs).
//
// Where (MatchBlock.h; free on rel-0.3.15, the integrator's assignment to this track):
//   +0xF4..+0xFF   shared: the tag (on), the game, the native cursor's follower, a page turn
//   +0x1B4..+0x1BF the two cursors, 6 bytes each: s16 x, s16 y, u8 buttons now, u8 buttons seen
namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::StageCursors
{
constexpr u32 SHARED = MatchBlock::STAGE_CURSORS_SHARED;
constexpr u32 SHARED_SIZE = MatchBlock::STAGE_CURSORS_SHARED_SIZE;
constexpr u32 CURSORS = MatchBlock::STAGE_CURSORS;
constexpr u32 CURSOR_SIZE = 6;
constexpr u32 CURSORS_SIZE = 2 * CURSOR_SIZE;
static_assert(CURSORS_SIZE == MatchBlock::STAGE_CURSORS_SIZE);
// While the two cursors run on this stage select (anything else: off).
constexpr u8 TAG_ON = 0xC2;

// The buttons the flows read, one bit each (the raw pad's).
enum Button : u8
{
  BTN_A = 0x01,
  BTN_B = 0x02,
  BTN_X = 0x04,
  BTN_Y = 0x08,
  BTN_START = 0x10,
  BTN_Z = 0x20,
};
u8 ButtonsOf(u16 pad_buttons);

// The game a layout is for (MatchBlock's ruleset ids).
constexpr u8 GAME_BRAWL = 1;
constexpr u8 GAME_PPLUS = 2;

// A rectangle in the cursor's units x 16: x0 <= x < x1, y0 <= y < y1 (up is +y).
struct Rect
{
  s16 x0, y0, x1, y1;
  constexpr bool Contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};
// A legal stage's tile: the ruleset's stage index (RankedSteps.h), its page, where it is.
struct Tile
{
  s8 stage;
  u8 page;
  Rect rect;
};
struct Point
{
  s16 x, y;
};

struct Layout
{
  u8 game = 0;
  // The cursor's range (the game's own: its cursor never goes past it), x 16.
  Rect bounds{};
  // How far a cursor moves in a frame at full tilt, x 16, and the stick's dead zone (of 128).
  s16 speed = 16;
  s16 dead_zone = 24;
  std::span<const Tile> tiles;
  // The page button (Brawl's Melee Stages / Brawl Stages); no pages: never hovered.
  bool pages = false;
  Rect page_button{};
  // Where each cursor starts.
  std::array<Point, 2> start{};
  // For the overlay only (floats, never synced): the picture's point (0..1 across, 0..1 down) of
  // the cursor's (0, 0), and how far one cursor unit goes across and down it.
  float u0 = 0.5f, du = 0.01f, v0 = 0.5f, dv = -0.01f;
};
const Layout& BrawlLayout();
const Layout& PPlusLayout();
const Layout* LayoutFor(u8 game);

// What a cursor is on: a legal stage's index, PAGE_BUTTON, or NOTHING.
constexpr int NOTHING = -1;
constexpr int PAGE_BUTTON = -2;
int Hovered(const Layout& layout, u32 page, int x, int y);
// The tile of `stage` on `page` (nullptr: not on that page).
const Tile* TileOf(const Layout& layout, int stage, u32 page);
// The middle of a rectangle, in the cursor's own units (a float for the game's cursor).
float MidX(const Rect& r);
float MidY(const Rect& r);

struct Cursor
{
  s16 x = 0;
  s16 y = 0;
  u8 now = 0;   // the buttons of the frame that just ran (the latch's)
  u8 seen = 0;  // the buttons the frame hook last saw
  bool operator==(const Cursor&) const = default;
};

struct State
{
  u8 game = 0;
  // The port whose cursor the game's own cursor follows (the last one that moved): its hover shows
  // the stage's preview.
  u8 follower = 0;
  // A page turn asked for: 0 none, 1 + the port that asked; and frames since (it gives up).
  u8 page_turn = 0;
  u8 page_frames = 0;
  std::array<Cursor, 2> cursors{};
  bool operator==(const State&) const = default;
};

// The two cursors, or nullopt while they don't run (the tag isn't there).
std::optional<State> Read(const GuestMemory& memory);
// Writes `state` with the tag on, only the bytes that differ. Returns bytes changed.
int Write(GuestMemory& memory, const State& state);
// The tag off (only if it was on). Returns bytes changed.
int Clear(GuestMemory& memory);
// A fresh pair of cursors at the layout's start points.
State Start(const Layout& layout);

// The presses `port` made since the frame hook last looked (edges of `now` against `seen`); they
// are seen from then on.
u8 TakePresses(State* state, int port);

// The latch's part: while the tag is on and the stage select runs, each of ports 1-2's raw sticks
// moves its own cursor (past the dead zone, full tilt = the layout's speed, integer math, clamped
// to the bounds) and its buttons are the frame's. Returns bytes changed.
int Latch(GuestMemory& memory, const std::array<std::optional<GCPadStatus>, 4>& raw);
// The CPU thread's latch (InputGate.h).
void LatchFrame(const Core::CPUThreadGuard& guard,
                const std::array<std::optional<GCPadStatus>, 4>& raw);

// ---- The overlay's view (UI only: published by the stage flows on first runs) ----
struct View
{
  bool on = false;
  u8 game = 0;
  u32 page = 0;
  struct Player
  {
    bool present = false;
    s16 x = 0, y = 0;
    s8 proposal = -1;  // the stage it proposes (ruleset index), -1 none
    bool local = false;
    std::string name;
  };
  std::array<Player, 2> players;
  s8 picked = -1;    // the stage the flow took (-1: not yet)
  s8 turn = -1;      // whose turn the steps say it is (-1: nobody's / both)
  // The step under way (Luis, 2026-10-05: "very obvious whose turn it is"): its kind
  // (Ranked::StepKind's number, 0: none), the strikes or bans its player still owes, a ban they may
  // leave out, Brawl's Meta Knight clause (one pick of any legal stage), a step for both players at
  // once (a casual game's pick), and the seconds its timer shows (-1: none).
  u8 kind = 0;
  u8 left = 0;
  bool optional = false;
  bool mk_clause = false;
  bool both = false;
  s16 seconds = -1;
  bool operator==(const View&) const = default;
};

// What a player's cursor says about the turn (the overlay's tag by it; UI only).
struct TurnCue
{
  enum class Mode : u8
  {
    Idle,     // nothing to say: no tag, no blink
    Acting,   // this player has to act: the cursor blinks, the tag says what to do
    Waiting,  // the local player waits for the other: a small grey tab, no blink
  };
  Mode mode = Mode::Idle;
  bool local = false;  // the cursor is this machine's player's
  std::string title;   // "YOUR TURN", "HBOX'S TURN", "WAITING FOR HBOX"
  std::string detail;  // "X STRIKES A STAGE · 2 LEFT" (may be empty)
  int seconds = -1;    // the turn's timer (-1: none)
  bool operator==(const TurnCue&) const = default;
};
// What `port`'s cursor (0 or 1) says, from this machine's side (the players' `local`): who acts in
// the step under way and how, in the cursors' tab names in capitals.
TurnCue TurnText(const View& view, int port);
// Brawl's Meta Knight clause, said loudly at the stage select's start: the banner's line under
// "META KNIGHT CLAUSE" ("HBOX PICKS ANY STAGE · NO STRIKES", or "YOU PICK ..." on the picker's
// screen); empty when the clause doesn't apply or the stage is picked.
std::string MkClauseNote(const View& view);
// Publish: every first-run boundary. Current: the view published at the boundary before the last
// (the frame the picture shows).
void Publish(View view);
View Current();
}  // namespace Orca::UX::StageCursors
