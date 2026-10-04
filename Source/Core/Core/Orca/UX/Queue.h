// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/MatchBlock.h"
#include "Core/Rollback/InputGate.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
}

// The queue's character select, Slippi-style (ORCA.md "The queue's character select"; YouGame
// docs/ORCA_ONLINE_UX.md; the `queue2` capability, the page's half in lib/orca-queue.ts):
//
// - **The queue's own character select.** With Anyone > Casual or Ranked lands on the local
//   Versus character select, where only the player's own panel plays: every other port is masked
//   entirely. The match block's header says so (MatchBlock::FLAG_SOLO, written at the player's
//   alone boundaries while the app has `queue2`), so the rules lock as in a queue room. A pick and
//   Start make the player ready: Orca prints `orca queue ready <casual|ranked> <char> <costume>`
//   and stays on the character select ("Searching for an opponent... B to cancel"); the page
//   searches. B (or another pick) while ready: `orca queue unready`, still on the character select.
//   B while not ready backs out to the menus as the game does (`orca menu cancel`). Start never
//   reaches the game here (one player can't start a versus match anyway).
// - **The match.** The page hosts or joins as before. The joiner's queue identity (QueueIdentity:
//   its pick, where its hand put the token in its own game, and the rating the page gave) rides the
//   session's value channel with its name, so from its plug frame on every machine has it at the
//   same frame: the host's game steers port 2's hand to that spot, puts the token down and turns
//   the costume (the input gate's synthesized stick and presses), and from then on game 1's
//   characters are locked (no token pickup, no costume change).
// - **The ready timer** (30 s): Start readies a player, B un-readies; both ready, Orca presses Start
//   for port 1 and the game goes on to the stage select (casual's two-turn pick, ranked's strikes).
//   Casual: when time is up the players who weren't ready are named (`orca queue timeout me|them`,
//   at a final frame) and nothing moves until the pages leave the room. Ranked: time up goes on
//   with the picks as they are. The timer runs in every ranked game but only in casual's first
//   (Timed()): from game 2 on, casual waits for both Starts with no clock and no timeout (Luis,
//   2026-10-04: more time to talk).
// - **Skip:** holding Z 1.5 s on the room's character select prints `orca queue skip` (the page
//   leaves the room and searches again). Local only: it changes nothing in the game.
// - **Later games:** casual comes back to the room's character select, characters free, ready
//   again with no timer; ranked runs the character order first (CharOrder.h), then the ready timer.
// - **After the room** (skip, timeout, the opponent gone, a set over): the session puts back this
//   player's own game as it was when the match came (OnlineMatch's queue image), so each player is
//   on their own queue character select with their pick, on port 1, and Orca prints
//   `orca queue ready` again if they were ready.
//
// What decides anything in the game is a pure function of emulated memory, the hook's frame number
// and the session's synced values (the plugged ports, each port's queue identity), written only
// where it differs, first runs and re-runs alike: the region at MatchBlock::QUEUE. Presses the
// game never sees (Start, B and Z are masked) reach it through the input gate's latch, which writes
// each frame's raw buttons of ports 1 and 2 into the region before the frame runs (InputGate.h).
// The lines on stdout and the overlay are local, from first runs only.
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::Queue
{
// ---- The region (MatchBlock::QUEUE, 0x30 bytes) ----
constexpr u32 READY = MatchBlock::QUEUE + 0x00;        // u8: bit p, port p (0, 1) is ready
constexpr u32 FLAGS = MatchBlock::QUEUE + 0x01;        // u8: GO, TIMED_OUT
constexpr u32 TIMEOUT_WHO = MatchBlock::QUEUE + 0x02;  // u8: bit p, port p wasn't ready in time
constexpr u32 LOCKED = MatchBlock::QUEUE + 0x03;       // u8: bit p, port p's game-1 pick is in
// u32: the hook frame + 1 the ready timer started at (0: not running); a function of the frame
// number, so a boundary run again computes the same.
constexpr u32 TIMER_START = MatchBlock::QUEUE + 0x04;
constexpr u32 RAW = MatchBlock::QUEUE + 0x08;       // u16[2]: ports 1, 2's raw buttons, last frame
constexpr u32 RAW_PREV = MatchBlock::QUEUE + 0x0C;  // u16[2]: the frame before
constexpr u32 TIMER = MatchBlock::QUEUE + 0x10;     // u16: frames into the ready timer (display)
constexpr u32 STEER = MatchBlock::QUEUE + 0x12;     // u16: frames port 2's pick has been steered
// Port 2's pick to put in (from its queue identity): u8 1, u8 character, u8 costume, u8 0, f32 x,
// f32 y (where its hand put the token in its own game).
constexpr u32 PICK = MatchBlock::QUEUE + 0x14;
// u32: the hook frame + 1 port 2's steering began at (0: not steering).
constexpr u32 STEER_START = MatchBlock::QUEUE + 0x20;
// u32: the hook frame + 1 port 2's token went down on its pick (0: not yet): the costume's turn.
constexpr u32 COSTUME_START = MatchBlock::QUEUE + 0x24;
// f32[2]: port 2's hand as the frame that just ran started (the latch's): A goes down only on a
// hand that stands still, since the hand slides on a little after the stick lets go.
constexpr u32 HAND_PREV = MatchBlock::QUEUE + 0x28;
constexpr u32 REGION_END = MatchBlock::QUEUE + 0x30;
static_assert(REGION_END <= MatchBlock::QUEUE + MatchBlock::QUEUE_SIZE);

constexpr u8 FLAG_GO = 0x01;         // both ready (or ranked's time up): Orca presses Start
constexpr u8 FLAG_TIMED_OUT = 0x02;  // casual's time up: frozen until the pages leave the room
// The queue's own character select: the B that stopped the search is still held, and the game
// doesn't see it until it is let go (else the token would come back up).
constexpr u8 FLAG_SWALLOW_B = 0x04;

// 30 s to ready up, 1.5 s of Z to skip, at the game's 60 frames a second.
constexpr int READY_FRAMES = 30 * 60;
constexpr int SKIP_FRAMES = 90;
// Steering port 2's hand gives up after this long (the player then picks by hand).
constexpr int STEER_LIMIT_FRAMES = 10 * 60;
// Port 2's stick stays centred this long first: the game takes a newly plugged pad's stick as its
// centre (the pad's origin), and a stick already steered then would read as pushed ever after (the
// hand drifted with the stick let go).
constexpr int STEER_SETTLE_FRAMES = 20;
// Turning port 2's costume (X) gives up after this long: the character is in, whatever the colour.
constexpr int COSTUME_LIMIT_FRAMES = 3 * 60;

// The character select's id for "no character" (muSelCharPlayerArea +0x1B8).
constexpr int NO_CHARACTER = 0x28;

// ---- The queue identity (Events::PortInfo::queue), 13 bytes, big-endian ----
//   +0 version 1   +1 u16 rating (0xFFFF none)   +3 u8 character (0xFF none)   +4 u8 costume
//   +5 f32 x   +9 f32 y   (the hand's place when it put the token down in the player's own game)
struct Identity
{
  int rating = -1;  // -1: none ("queue rating -")
  int character = -1;  // the character select's id (+0x1B8); -1 none
  int costume = 0;     // the colour number (+0x1BC)
  float x = 0;
  float y = 0;
  bool HasPick() const { return character >= 0 && character != NO_CHARACTER; }
  bool operator==(const Identity&) const = default;
};
std::vector<u8> EncodeIdentity(const Identity& identity);
std::optional<Identity> DecodeIdentity(const std::vector<u8>& bytes);

// ---- What a boundary sees ----
struct CssPort
{
  bool readable = false;  // the area could be read
  bool human = false;
  int character = NO_CHARACTER;  // under the hand while the token is in it, else the token's
  bool placed = false;           // the token is down on a character
  int costume = 0;
  // The hand (OnlineRules.h CssHand): what it points at and where.
  u32 hand_target = 0;
  float hand_x = 0;
  float hand_y = 0;
  bool operator==(const CssPort&) const = default;
};

struct View
{
  bool queue2 = false;  // the header is the queue's (FLAG_QUEUE2) and locks
  bool solo = false;    // the queue's own character select (FLAG_SOLO)
  bool ranked = false;
  bool css = false;      // on the character select
  bool game1 = true;     // no fight since the header was written
  bool order_done = true;  // ranked later games: the character order is done (CharOrder.h)
  u8 plugged = 0;        // the ports in play (MatchBlock::PLUGGED)
  std::array<CssPort, 2> ports{};
  std::array<u16, 2> raw{};
  std::array<u16, 2> raw_prev{};
  // Port 2's hand where the frame that just ran started (the latch's HAND_PREV).
  float hand2_prev_x = 0;
  float hand2_prev_y = 0;
  std::optional<Identity> pick2;  // port 2's queue identity's pick, from its plug frame on
  bool operator==(const View&) const = default;
};

struct State
{
  u8 ready = 0;
  u8 flags = 0;
  u8 timeout_who = 0;
  u8 locked = 0;
  u32 timer_start = 0;
  u16 timer = 0;
  u16 steer = 0;
  u32 steer_start = 0;
  u32 costume_start = 0;
  bool pick_set = false;
  u8 pick_character = 0;
  u8 pick_costume = 0;
  float pick_x = 0;
  float pick_y = 0;
  bool operator==(const State&) const = default;
};

// The room's character select with both players in: the ready step (game 1, and later games once
// ranked's character order is done).
bool ReadyPhase(const View& view);

// Whether the ready step has its 30 s timer: every ranked game, and casual's first only (game 1:
// strangers who just met). Casual's later games with the same opponent wait for both Starts, with
// no clock and no timeout (Luis, 2026-10-04: "a little bit more time to be social"). A function of
// the match block (the header's mode and FOUGHT), so the same on every machine and re-run.
bool Timed(const View& view);

// The state after this boundary, from the state before it, the view and the hook's frame number.
// Pure and idempotent (the same view and state give the same result, and a result fed back in
// changes nothing).
State Advance(const View& view, const State& state, int frame);

// The gate's masks and presses for the frame about to run. Pure.
Rollback::InputGate::Masks Gate(const View& view, const State& state);

// The skip's hold (local, first runs only): Z held SKIP_FRAMES boundaries in a row while a skip may
// be asked for fires once; Z must be let go before the next.
struct SkipHold
{
  int frames = 0;
  bool sent = false;
  // One first-run boundary: whether a skip is asked for now.
  bool Step(bool may_skip, bool z_held);
  // 0..1 of the way to a skip, for the overlay (0 while nothing is held or once it fired).
  float Progress() const;
};

// ---- Fixed point (the steering's arithmetic) ----
// Every machine computes port 2's stick from the hand's place in emulated memory, so the steering
// is integer arithmetic only (ORCA.md "Host arithmetic"): host floating point differs between the
// builds. Apple clang fused the old float version's dx * dx + dy * dy into one FMA on ARM64, which
// MSVC rounds as two products and a sum on x86-64; 12 of 20 million random (hand, pick) pairs then
// gave sticks a unit apart, a desync no synctest on one machine can see. A place is in FIXED_ONEs,
// 1/65536 of a unit.
constexpr int FIXED_BITS = 16;
constexpr s64 FIXED_ONE = s64{1} << FIXED_BITS;
// Places this far out or more are no place (the character select spans about -30..30, -20..20).
constexpr s64 FIXED_LIMIT = s64{4096} << FIXED_BITS;
// A float's value in FIXED_ONEs, truncated toward zero, worked out from its bits alone (no float
// instruction, so every host and compiler agrees); nullopt for a NaN, an infinity, or a magnitude
// of FIXED_LIMIT or more.
std::optional<s64> ToFixed(float value);
// floor(sqrt(n)), exactly.
u64 IntSqrt(u64 n);

// Where port 2's stick goes to take its hand from (x, y) to (tx, ty): full tilt when far, gentler
// near, centred within reach (or when a place is no place). Pure, and integer only: the same bytes
// on every host.
std::pair<u8, u8> SteerToward(float x, float y, float tx, float ty);
// Whether port 2's hand has all but stopped: under 0.05 a frame on both axes since the frame began
// (Brawl's hand creeps about 0.03 a frame with the stick let go). Integer only, as SteerToward.
bool HandStill(float x, float y, float prev_x, float prev_y);

// ---- Memory ----
View ReadView(const GuestMemory& memory, const std::vector<Events::PortInfo>& ports);
State ReadState(const GuestMemory& memory);
// Writes the region's state fields (not RAW/RAW_PREV: the latch's), only where they differ.
int WriteState(GuestMemory& memory, const State& state);
// The latch (InputGate::SetLatch): ports 1 and 2's raw buttons into RAW, the old RAW into RAW_PREV,
// and port 2's hand into HAND_PREV, while the header is the queue's. Returns bytes changed.
int Latch(GuestMemory& memory, const std::array<std::optional<GCPadStatus>, 4>& raw);

// ---- The frame hook's part (UX.cpp), after the character order ----
// Reads, advances and writes the state (first runs and re-runs); on first runs prints the lines,
// follows Z for a skip and gives the overlay its lines.
void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports, bool alone);
// The input gate's part: masks from memory alone.
Rollback::InputGate::Masks GateFrame(const Core::CPUThreadGuard& guard);
// Whether the queue owns the character select's Start (a queue2 room's character select): the
// character order's own Start is then the ready timer's to give.
bool OwnsStart(const Core::CPUThreadGuard& guard);
// The input gate's latch.
void LatchFrame(const Core::CPUThreadGuard& guard,
                const std::array<std::optional<GCPadStatus>, 4>& raw);

// ---- The app's side (any thread) ----
// With Anyone > Casual or Ranked picked with the app's `queue2`: the player waits on the queue's own
// character select (the header the session asks for: OnlineMatch's WantedRulesMode).
void Begin(bool ranked);
// Backed out to the menus (`orca menu cancel`).
void End();
bool Active();
bool Ranked();
// `queue rating <n|->`: this player's rating in the current queue (-1 none).
void SetOwnRating(int rating);
int OwnRating();
// The last alone first-run boundary saw the queue's own character select with the player ready.
bool SoloReady();
// The last alone first-run boundary saw the queue's own character select.
bool OnSoloCss();
// The session put back the player's own game after a room (OnlineMatch's queue image). `keep_ready`
// false (this player's own ready timer ran out): the ready flag is cleared at the next boundary, so
// the search doesn't start again by itself.
void AfterRestore(bool keep_ready);
// The app's `queue-cancel`: its search ended, so on the queue's own character select the player
// isn't ready any more (cleared at the next alone boundary; `orca queue unready` follows).
void ClearReady();
// The casual ready timer that ran out and its frame is final (OnlineMatch's ConfirmResults): this
// player's port didn't ready (`me`) or the opponent didn't (`them`); printed once.
void ConfirmTimeout(int confirmed, int local_port);
// Whether the last `orca queue timeout` printed said `me` (once; cleared as it is read).
bool TakeTimeoutWasMine();
}  // namespace Orca::UX::Queue
