// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
// The reader follows the menu at every frame boundary that runs a frame for the first time (never a
// re-run: what a rollback undoes was never decided) and announces a pick only when it happens while
// this game plays alone (Events' `alone`): a pick while friends play is theirs too, so is one made
// while a friend is about to land (an arrival or a prepare-join not taken yet, or an invite's
// stored keyframe that a friend who arrives would replay from), and one made while the app's own
// join or leave waits, or seen while a friend catches up or a session runs on guessed inputs, is
// not news once the game is alone again. Whenever the game stops following on from what the reader
// saw (Events NoteResync: back to solo after a session, a friend's keyframe loaded) it starts over
// from the next boundary without announcing anything, so nothing stale is ever printed.
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
// select, where the app's queue searches while the player waits ("online casual", "online ranked",
// with the app's "host"; without it, "... local": nothing searches), With Friends on the same
// screen, where friends drop in.
std::string_view MenuEventText(OnlinePick pick, bool queue = false);

// The current scene's name ("" when there is none to read).
std::string ReadSceneName(const GuestMemory& memory);

// The search after a Casual or Ranked pick (ORCA.md "Matchmaking"), as the overlay shows it: the
// app's queue searches until it matches this game (`host <code>`: it hosts, Found until the
// opponent plugs in; `join <code>`: the join's own messages take over), the app ends the search
// (`queue-cancel`), or the player backs out of the character select to the main menu, which
// prints "orca menu cancel" once. Any thread.
namespace Search
{
enum class State
{
  None,
  Searching,
  Found,
};
void Begin(bool ranked);
// The app's host/join for the match: Found (hosting) or None (joining).
void Matched(bool hosting);
// The opponent plugged in, the app ended the search, or the room it waited in went away.
void End();
State Current();
bool Ranked();
// The overlay's lines while searching or found (empty: nothing).
std::pair<std::string, std::string> Lines();
}  // namespace Search

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
