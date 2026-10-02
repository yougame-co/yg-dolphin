// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

// The game's own Online menu (ORCA.md "Online menu"; the patches that open it are in
// Data/Sys/Orca/<profile>.patches): Brawl's Wi-Fi button, Project+'s Play Online, opens the
// game's ONLINE page with no connection, and every way on from it leaves the main menu through the
// game's exit to the local Versus character select, with the main menu's exit code telling which:
// 30 Casual (With Anyone's Basic Brawl / Basic Versus), 31 Ranked (Team Battle), 24-27 a With
// Friends mode. The app hears of the pick on stdout ("orca menu online casual local", ...), so the
// page can say what comes next; nothing here changes the game.
//
// The reader follows the menu at every frame boundary that runs a frame for the first time (never
// a re-run: what a rollback undoes was never decided) and announces a pick only when it happens
// while this game plays alone (Events' `alone`): a pick while friends play is theirs too, and a
// pick seen while a friend catches up or a session runs on guessed inputs is not news once the
// game is alone again. Whenever the game stops following on from what the reader saw (Events
// NoteResync: back to solo after a session, a friend's keyframe loaded) it starts over from the
// next boundary without announcing anything, so nothing stale is ever printed.
//
// Found under the harness (ORCA_UX_PROBE watches, Brawl rev 2 and Project+ alike, which run the
// same executable and scene code): the scene manager (0x805A0060) +4 -> the scene, whose +0 is its
// name: "muMenuMain" on the main menu, "scMemoryChange" between scenes; +0x284 the main menu's exit
// code, written as the menu leaves (it reads 0 on the menu itself and from the next scene on).
namespace Orca::UX
{
class GuestMemory;

enum class OnlinePick
{
  Casual,
  Ranked,
  Friends,
};

// What one frame boundary shows of the main menu.
struct MenuState
{
  enum class Where
  {
    Other,
    Menu,     // muMenuMain
    Leaving,  // scMemoryChange: between two scenes
  };
  Where where = Where::Other;
  u32 exit_code = 0;
};

MenuState ReadMenuState(const GuestMemory& memory);

// The pick an exit code stands for, if any.
std::optional<OnlinePick> PickForExitCode(u32 exit_code);

// The stdout line's words after "orca menu": Casual and Ranked land on the local Versus character
// select for now ("local": nothing searches), With Friends on the same screen, where friends drop
// in.
std::string_view MenuEventText(OnlinePick pick);

class OnlineMenuReader
{
public:
  // One frame boundary. `resyncs` is Events::Resyncs(). Returns the pick to announce, if any.
  std::optional<OnlinePick> Boundary(const MenuState& state, bool resimulating, bool alone,
                                     u64 resyncs);

private:
  bool m_started = false;
  u64 m_resyncs = 0;
  // The last scene before this one (or this one) was the main menu.
  bool m_from_menu = false;
  // This exit's code was seen (announced or not).
  bool m_seen = false;
};

// The frame hook's part: reads the menu and prints "orca menu ..." (Orca::Status::Menu). Reads
// emulated memory only.
void ReadOnlineMenu(const Core::CPUThreadGuard& guard, bool resimulating, bool alone);
}  // namespace Orca::UX
