// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/OnlineRules.h"
#include "Core/Rollback/InputGate.h"

namespace Core
{
class CPUThreadGuard;
}

// No CPUs online, and only the seats in use on the screen (ORCA.md "No CPUs online"; `ux=19`).
//
// Luis, 2026-10-05: "If you go into any other mode and create a CPU, and then you exit and go into
// ranked, that CPU actually remains, and it creates a glitched illegal condition. Whenever we're in
// an online mode, we should be clearing out any bots and continuing to disable it. ... If I go into
// casual or ranked, I only see one character possible, which is me. When someone else joins, they
// take the player 2 slot."
//
// How the CPU got there (found under the harness, q12): the Versus character select builds its
// panels from a record the game keeps between visits, gmSelCharData ([0x805A00E0]+0x10; each
// port's +0xB8 + port x 0x5C: +0 the character, +1 the player's state, 0 a human, 1 a CPU, 3
// nobody), and saves its panels back into it only as it goes on to the stage select. Vanilla Brawl
// resets that record whenever Versus starts from the main menu; Project+ doesn't, so a CPU from a
// local game that reached the stage select came back on With Anyone's Casual and Ranked selects and
// on With Friends', and went into the queue match: a third fighter in game 1 (a CPU on panel 3), or
// a room whose joiner could never take panel 2 (a CPU there) and never left its character select.
// Even in Brawl, A on an empty panel's player-type button made a CPU on With Friends' select.
//
// What is online here (Online()): the match block's header locks (the queue's own character select
// or a casual or ranked queue room, OnlineRules.h), or the main menu's pick Orca keeps in the
// running sqVsMelee (+0x18; CssTitle::ReadPick) is With Friends (24-27) or With Anyone's Casual (30)
// or Ranked (31), or two ports or more are plugged in (a friend who dropped in, wherever the host
// was). Group > Brawl alone, and everything else offline, keep CPUs as the game has them.
//
// While a select is online, at every frame boundary, first runs and re-runs, writing only bytes that
// differ (a pure, idempotent function of emulated memory and the synced ports):
// - **The record** (ClearRecord), between two scenes (scMemoryChange) only: a CPU's state becomes
//   nobody's, and so does a human's on a port nobody plugged in (a second local player's, kept by
//   Project+). The first such boundary after the menu's pick comes before the character select
//   builds its panels (5 boundaries before it in Project+, 33 in Brawl), so the select starts with
//   only the players who are there. Never on the stage select or in a fight: the stage select's end
//   copies the record into the fight's players, and by the next boundary that copy is made, so no
//   fight ever loses a player to this.
// - **The panels** (ClearPanels), on the character select: a CPU's panel (its area's kind +0x1B4, 2)
//   becomes an empty one (0). The select saves that kind into the record as it leaves, so the CPU
//   plays no fight. The panel's own picture isn't the game's empty one then (its CPU plate stays),
//   which is why the seats below hide it, or the join below makes it a player's. A human's panel
//   whose port nobody plugged in is the game's: it empties one itself 17 frames after its
//   controller goes (both games, the token back in the hand and off the grid), and builds none for
//   a port without a controller; a write here would come first and leave the token's highlight on
//   the grid (found under the harness, q12).
// - **The buttons** (GateMasks): where no header locks (With Friends, a friend's drop-in, Casual or
//   Ranked before the queue's header lands), A never reaches a player-type button (0x1D: a CPU of an
//   empty panel, or of a human's own) or a name button (0x1C), nor anywhere within one frame's
//   travel of them (TYPE_BUTTONS_A_FLOOR). Under a header the online rules already pass A only in
//   the grid. Joining a fresh panel needs no A: a hand that moves into the grid with its token joins
//   it. A panel a CPU had (its token left on the grid, gone with the CPU) can't be joined that way:
//   Orca presses A for its port while that port's hand rests on the panel's own player-type button
//   (PressesJoin), which the game turns into the player's panel, as a local player rejoins after
//   removing a CPU. So a friend who plugs into a live select whose CPU was on their panel takes it
//   at once.
// - **The seats** (ApplySeats), on the character select: an empty panel (kind 0) whose port nobody
//   plugged in is hidden, its 39 game models (MuObjects at area +0xB0..+0x14C; each one's nw4r
//   ScnMdl at +0xC) drawn neither opaque nor translucent (ScnObj flags +0xCC, 0x60 over the game's
//   0xA0000000). So the queue's own select shows the player alone, with the opponent's seat
//   (SEARCHING, the kit's ring) once it searches; a queue room shows both players; With Friends
//   shows the players who are there, a friend's panel appearing as they plug in. Hiding is drawing
//   only: the panel's logic and its buttons stay (the A masks above keep them dead). Any panel
//   that should show and carries Orca's 0x60 gets it cleared, on any character select, so a select
//   that stops being online (the friend left Group > Brawl) shows its panels again.
namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::OnlineSeats
{
// ---- The menu's picks (sqVsMelee +0x18, RSBE01.patches) ----
constexpr u32 PICK_FRIENDS_FIRST = 24;
constexpr u32 PICK_FRIENDS_LAST = 27;
constexpr u32 PICK_CASUAL = 30;
constexpr u32 PICK_RANKED = 31;

// ---- gmSelCharData's players (both games) ----
constexpr u32 GLOBAL_SEL_CHAR = 0x10;  // [0x805A00E0] + 0x10: gmSelCharData*
constexpr u32 RECORD_PLAYERS = 0xB8;
constexpr u32 RECORD_PLAYER_SIZE = 0x5C;
constexpr u32 RECORD_STATE = 0x01;
constexpr u8 STATE_HUMAN = 0;
constexpr u8 STATE_CPU = 1;
constexpr u8 STATE_NOBODY = 3;

// ---- The character select's player areas (muSelCharPlayerArea, task +0x44 + 4 x port) ----
constexpr u32 AREA_KIND_NONE = 0;
constexpr u32 AREA_KIND_HUMAN = 1;
constexpr u32 AREA_KIND_CPU = 2;
constexpr u32 AREA_MODELS = 0xB0;  // 40 MuObject pointers (one slot unused)
constexpr u32 AREA_MODEL_COUNT = 40;
constexpr u32 MUOBJECT_SCENE_MODEL = 0x0C;  // nw4r::g3d::ScnMdl*
constexpr u32 SCENE_MODEL_FLAGS = 0xCC;     // nw4r::g3d::ScnObj's option flags
constexpr u32 MODEL_FLAGS_GAME = 0xA0000000;  // every panel model, both games
constexpr u32 MODEL_FLAGS_HIDDEN = 0x00000060;  // DISABLE_DRAW_OPA | DISABLE_DRAW_XLU

// ---- The buttons (both games, the 2026-10-03 rasters: css-gate-runs raster.log, pplus-map.log) ----
// The player-type and name buttons start at y -18.3 or lower (between -17.4 and -18.3) in both
// games; the hand moves at most 1.0 a frame (Brawl 0.83). From -16.4 or above, no single frame's
// stick takes it onto one with A down. Brawl's panel picture (a costume, 0x1B) ends at -17.4,
// Project+'s at -16.6, so A below -16.4 loses only the picture's bottom edge (X and Y still turn the
// costume).
constexpr float TYPE_BUTTONS_A_FLOOR = -16.4f;
constexpr u32 CSS_BUTTON_NAME = 0x1C;

// ---- Pure decisions ----
// Whether a select is online: a header that locks, an online pick, or two ports or more plugged in.
bool Online(const Rules::Header& header, u32 pick, int players);
// A port's state in the record while online: a CPU's, and a human's on a port nobody plugged in,
// become nobody's; anything else stays.
constexpr u8 RecordStateFor(u8 state, bool plugged)
{
  return state == STATE_CPU || (state == STATE_HUMAN && !plugged) ? STATE_NOBODY : state;
}
// A panel's kind while online: a CPU's becomes empty; a human's stays, plugged in or not (above).
constexpr u32 PanelKindFor(u32 kind)
{
  return kind == AREA_KIND_CPU ? AREA_KIND_NONE : kind;
}
// Whether port `port`'s (0-3) panel is hidden: online, empty, nobody plugged into it, and not the
// opponent's seat of a queue select (port 2: always in a queue room, and on the queue's own select
// while it searches).
bool SeatHidden(bool online, const Rules::Header& header, int port, u32 kind, bool plugged,
                bool searching);
// Whether A may reach the game from this hand on an online select without a header (above). An
// unreadable hand may not (the select being built or torn down), as under a header
// (Rules::CssHandMayPressA).
bool HandMayPressA(const Rules::CssHand& hand);
// Whether Orca presses A for port `port` (0-3, plugged in) on an online select without a header:
// its own panel is empty (`kind` 0) with its token neither in the hand nor flying (a CPU's panel
// once it is empty, whether the game or ClearPanels emptied it; a fresh empty panel holds its token
// in the hand), and the port's hand rests on that panel's own player-type button (where a
// player's hand starts). A there makes the panel the player's (HMN), the game's own way back from a
// CPU's; nothing else joins it (the hand moving into the grid doesn't, with no token in it).
bool PressesJoin(const Rules::CssHand& hand, int port, u32 kind, bool token_in_hand,
                 bool token_flying);

// ---- Memory ----
// The ports plugged in (bit n: port n+1) as `memory` keeps them for the input gate: the friends
// bytes (MatchBlock FRIENDS_SEEN, written at every boundary once a friend has plugged in), else
// port 1 alone.
u8 PluggedInMemory(const GuestMemory& memory);
// Online() for this memory and these ports.
bool ReadOnline(const GuestMemory& memory, u8 plugged);
// The record's clear (between two scenes, online only). Returns the bytes changed.
int ClearRecord(GuestMemory& memory, u8 plugged);
// The panels' clear (the character select, online only). Returns the bytes changed.
int ClearPanels(GuestMemory& memory, u8 plugged);
// The seats (any character select). Returns the flag words written; `hidden` gets the hidden
// panels (bit p).
int ApplySeats(GuestMemory& memory, u8 plugged, u8* hidden = nullptr);
// What the input gate masks for the frame about to run (reads only).
Rollback::InputGate::Masks GateMasks(const GuestMemory& memory);

// ---- The frame hook's parts (UX.cpp), first runs and re-runs, Brawl rev 2 and Project+ ----
// `plugged`: the session's ports for the frame about to run (bit n: port n+1), as the online rules
// take them.
// After the online rules (the header this boundary called for is in memory): the record and the
// panels.
void ClearFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating, u8 plugged);
// After the queue (its ready flag says whether the queue's own select searches): the seats.
void SeatsFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating, u8 plugged);
}  // namespace Orca::UX::OnlineSeats
