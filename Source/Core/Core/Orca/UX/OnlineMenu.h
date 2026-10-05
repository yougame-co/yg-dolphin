// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"

namespace Core
{
class CPUThreadGuard;
}

// The game's own Online menu (ORCA.md "Online menu"; the patches that open it are in
// Data/Sys/Orca/<profile>.patches): Brawl's Wi-Fi button, Project+'s Play Online, opens the
// game's ONLINE page with no connection, and every way on from it leaves the main menu through the
// game's exit to the local Versus character select, with the main menu's exit code telling which:
// 30 Casual (With Anyone's Basic Brawl / Basic Versus), 31 Ranked (Team Battle), 25 With Friends
// (24-27 were the With Friends page's buttons, all alike; since `ux=18` A on With Friends leaves
// with 25 and that page never opens). The app hears of the pick on stdout ("orca menu online
// casual local", ...), so the page can say what comes next; nothing here changes the game.
//
// The reader follows the menu at every frame boundary that runs a frame for the first time (never a
// re-run: what a rollback undoes was never decided) and announces a pick itself only when it
// happens while this game plays alone (Events' `alone`). A pick made while the game isn't alone
// (friends play, or one is about to land: an arrival or a prepare-join not taken yet, an invite's
// stored keyframe that a friend who arrives would replay from; or the app's own join or leave
// waits) is never printed later by the reader, now or once the game is alone again. Casual or
// Ranked made so goes to the session once, at the boundary that saw it (TakeLobbyPick below, Luis
// 2026-10-05: they drop the friends lobby): a host leaves its friends and announces it
// (AnnounceOnlinePick), a joiner lets it go, and its host's leaving brings it home on the character
// select that pick took both games to, where it announces the pick that select holds (CssPick).
// Acting on a first run's pick while the session's inputs may still be guessed is safe there: the
// session ends at that boundary, so what the host's solo game does next follows its own view of the
// game. With Friends while friends play stays theirs. Whenever the game stops following on from
// what the reader saw (Events NoteResync: back to solo after a session, a friend's keyframe loaded)
// it starts over from the next boundary without announcing anything, so nothing stale is ever
// printed.
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
  // The pick the last Boundary saw made while this game was not alone, which it never announces
  // (the session decides what it means: TakeLobbyPick below). Once; the next Boundary forgets it.
  std::optional<OnlinePick> TakeBusyPick();

private:
  bool m_started = false;
  u64 m_resyncs = 0;
  // The last scene before this one (or this one) was the main menu.
  bool m_from_menu = false;
  // This exit's code was seen (announced or not).
  bool m_seen = false;
  std::optional<OnlinePick> m_busy;
};

// ---- Leaving the friends lobby for the queue, and coming home (ORCA.md "Drop-in", "Online
// menu"; Luis, 2026-10-05: Casual and Ranked under With Anyone drop the friends lobby for a
// 2-player room the matchmaker fills, and a guest whose host left goes back to their own lobby) ----

// Prints "orca menu online ..." for a pick (with the app's "host": "online casual" / "online
// ranked", else "... local") and starts what it means: the queue's own character select (with the
// app's "queue2"), the search, or neither (Friends). The reader's own announcement, and the
// session's for a pick it took out of the friends lobby (TakeLobbyPick).
void AnnounceOnlinePick(OnlinePick pick);

// A With Anyone pick (Casual or Ranked) the frame hook saw at this boundary while the game was not
// alone (friends in it or about to land), which nothing printed: the session takes it at the same
// boundary, after the hook (Rollback/OnlineMatch.cpp). A host leaves its friends lobby for the queue
// and then announces it; a joiner's game is its host's, so it lets it go. Once. CPU thread.
std::optional<OnlinePick> TakeLobbyPick();

// Whether a scene is "the menus" for a former joiner to move to port 1 there (Rollback/
// OnlineMatch.cpp ComeHome): the main menu (every page of it) or a character select, never a fight,
// a stage select, a results screen or the moment between two scenes, and never inside a
// single-player mode (SequenceHoldsDropIn). Pure.
bool MenusScene(std::string_view scene, std::string_view sequence);
// MenusScene as the frame hook read it at this boundary (first runs; false before the first, and
// for any game but Brawl rev 2 and Project+). CPU thread.
bool OnTheMenus();

// The With Anyone pick (Casual or Ranked) the character select the frame hook read at this boundary
// was opened for (the main menu's exit code Orca keeps in the running sqVsMelee,
// CssTitle::ReadPick: 30, 31), nullopt anywhere else (another scene, With Friends' character
// select, a Versus one opened otherwise). Where a former joiner that comes home there picks up that
// pick (Rollback/ OnlineMatch.cpp ComeHome). First runs, as OnTheMenus. CPU thread.
std::optional<OnlinePick> CssPick();
// Whether a Casual or Ranked pick the session couldn't take at the boundary that saw it (Rollback/
// OnlineMatch.cpp TakeLobbyPickNow) still stands where the frame hook last read the game: on the
// way out of the main menu (between two scenes) or on the character select that pick opened
// (CssPick). Anywhere else (back on the main menu, a stage select, a fight) it is stale. CPU
// thread.
bool PickStands(OnlinePick pick);
// The pick a character select's kept exit code stands for when it is Casual or Ranked (30, 31).
// Pure.
std::optional<OnlinePick> QueuePickForCssCode(u32 code);

// The frame hook's part: reads the menu and prints "orca menu ..." (Orca::Status::Menu). Reads
// emulated memory only.
void ReadOnlineMenu(const Core::CPUThreadGuard& guard, bool resimulating, bool alone);

// ---- Friends drop in from the menus (ORCA.md "Online menu", "Drop-in") ----
//
// Both games boot to the main menu (Data/Sys/Orca/<profile>.patches). A friend who plugs in while
// the host is in the menus goes with the host to With Friends' character select, the place friends
// play, as if the host had picked With Friends; one who arrives while the host plays a
// single-player mode waits until the host is back (the session holds the join: Rollback/
// OnlineMatch.cpp, `orca state friend-holding` / `friend-waiting`).
//
// The scene manager (gfSceneManager, 0x805A0060): +0x4 the scene, +0x10 the sequence (each with its
// name at +0), +0x284 the exit code a scene leaves with, +0x288 its process step. Every menu exit
// in the game (muMenuMain's included) is those two stores: the code, then step 2, after which the
// manager ends the scene and the sequence's setNext reads the code (sqMenuMain's sends 24-27 to
// sqVsMelee through Orca's block, ORCA.md "Online menu").

// The current sequence's name ("sqMenuMain", "sqVsMelee", ...; "" when there is none to read).
std::string ReadSequenceName(const GuestMemory& memory);

// A sequence a friend never drops into: the single-player modes (Classic, All-Star, Boss Battles,
// Events, Training, Home-Run Contest, Target Smash, Multi-Man Brawl, the Subspace Emissary) and
// the Vault's screens. Everything else takes a friend in (the menus, Versus and its relatives, the
// boot); a friend who plugged in during the boot goes to With Friends once the main menu runs.
bool SequenceHoldsDropIn(std::string_view sequence);

// The host's game, as its last first-run boundary read it: in a sequence that holds a drop-in.
// Any thread (the session reads it at the next boundary, after the frame hook).
bool DropInHeld();

namespace FriendsMove
{
// The manager's process step while a scene runs, and the step that ends it (the menu's own exit).
constexpr u32 STEP_RUNNING = 1;
constexpr u32 STEP_EXIT = 2;
// With Friends (once the With Friends page's FIGHT!, the friend list's Brawl, the code A on the
// ONLINE page's With Friends leaves with since `ux=18`): sqVsMelee, and B from its character select
// comes back to the ONLINE page with With Friends selected.
constexpr u32 EXIT_WITH_FRIENDS = 25;
// No friend plugs in before this frame: a host's keyframe is never of its first 300 frames
// (Rollback/OnlineMatch.cpp FIRST_KEYFRAME_FRAME), and a friend plugs in after replaying from one.
constexpr int FIRST_FRIEND_FRAME = 300;
// Frames the main menu runs, built, with a friend waiting before both go: the friend's game shows
// the host's menu for half a second first.
constexpr u32 MENU_SETTLE_FRAMES = 30;
// muMenuMain's byte at +0xAC8 (sora_scene, the same in both games; found under the harness): its
// start sets it 0, and the menu's process loads its files first, frame after frame, then builds
// every page in one frame (the 29 page procs at +0x35C and the rest it destroys on its exit) and
// sets it 1. Leaving a menu that hasn't built yet runs its exit (0x806CEBF8) over a proc table
// that still holds the heap's fill (0xCCCCCCCC), which calls through a null vtable: Brawl reads
// 0x00000008 at 0x806CEC6C and never draws another frame. How long the files take is the game's
// own: 58 frames from the step going 1 on the boot's first menu, 121 on one coming back from the
// character select, 1 in Project+. It means nothing at step 0 (a menu that hasn't started still
// has the last menu's byte there).
constexpr u32 MENU_BUILT = 0xAC8;

// What one boundary reads.
struct Inputs
{
  // The block's memory is there (MatchBlock::Present and the friends bytes mapped).
  bool present = false;
  // The ports plugged in at this frame (bit n: port n+1).
  u8 plugged = 0;
  // A queue room's header (casual or ranked): its opponent plugs in on the character select, and a
  // queue match never goes to With Friends.
  bool queue = false;
  std::string scene;
  std::string sequence;
  u32 step = 0;
  u32 exit_code = 0;
  // On muMenuMain: its MENU_BUILT byte is set (the pages are built; only then can it be left).
  bool menu_built = false;
  int frame = 0;
  // The block's friends bytes (MatchBlock FRIENDS_*).
  u8 tag = 0;
  u8 seen = 0;
  u8 pending = 0;
  // The frame + 1 from which the main menu has run, built, with a friend pending (0: it hasn't).
  u32 menu_since = 0;
};

// What that boundary writes.
struct Outputs
{
  // Write the friends bytes (with the tag): false while nothing was kept and no friend is plugged in
  // (solo play never writes them).
  bool keep = false;
  u8 seen = 0;
  u8 pending = 0;
  u32 menu_since = 0;
  // Leave the main menu for With Friends: the exit code and STEP_EXIT, as the menu's own exit.
  bool move = false;
};

// Game logic, a pure function of what it reads: run again at the same boundary on what it wrote, it
// writes the same (a rollback's re-run from the snapshot saved there).
Outputs Decide(const Inputs& in);

// The frame hook's part, first runs and re-runs alike, Brawl rev 2 and Project+ only. Returns
// whether the host left the main menu for With Friends at this boundary.
bool Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports);
}  // namespace FriendsMove
}  // namespace Orca::UX
