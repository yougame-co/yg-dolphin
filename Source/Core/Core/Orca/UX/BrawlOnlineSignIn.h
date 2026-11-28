// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Orca::BrawlOnline
{
class SignInFlow;
}

// The Brawl Online sign-in screen, drawn by the overlay over the game: one panel driven by
// BrawlOnline::SignInFlow (Core/Orca/BrawlOnlineAccount.h) with email and password fields, Sign in,
// Create account (with a display name), Forgot password? (live builds only), Cancel, and the
// "check your email, then press Continue" state. Every network call runs on a worker thread; the
// video thread only draws and polls.
//
// Up and Down move between the panel's rows as drawn, Left and Right along a row of buttons, Tab
// through every item in order; Enter presses the focused one (in a field, the screen's main button),
// Escape cancels. The app's pads do the same (UX/PadKeys.h): the D-pad or the stick moves, A
// presses, B cancels; typing still needs the keyboard. While it is open the keyboard is
// captured (UX/TextEntry.h): the game, Orca's shortcuts and its own keys see none of it, and every
// pad reads neutral to the game. So it never opens while online play runs (OnlineActive), and closes at the next frame if
// online play starts under it (a friend dropping in): neutral pads would reach the other players as
// real input.
// Host-only: nothing here reads or writes emulated state.
namespace Orca::UX::BrawlOnlineSignIn
{
// Runs on the video thread once the flow saved user.json, with its path.
using Done = std::function<void(const std::string& user_json_path)>;

// Opens the screen for the YouGame player `player_id` named `yougame_name`, replacing one already
// open. When this PC already keeps that player's user.json it opens nothing and returns the path:
// a player signs in once per PC. Returns empty otherwise (the screen opened, or nothing can open for
// an empty or dev id). Any thread.
std::string Open(std::string_view player_id, std::string_view yougame_name, Done done = {});
// The same on `flow` (BrawlOnline::OpenSignIn's), from its first state. False, opening nothing,
// for no flow or while online play runs. Any thread.
bool OpenFlow(std::unique_ptr<BrawlOnline::SignInFlow> flow, Done done = {});
bool IsOpen();
// Closes the screen. What was typed is wiped at once (after any Draw in progress), and ImGui's
// copies of it at the video thread's next Draw. A call in flight finishes on its own and saves
// nothing (SignInFlow::Cancel). Any thread, the Done callback included.
void Close();
// The overlay's draw (video thread, inside the ImGui frame). Nothing while closed.
void Draw();

// The plain line under the title. In a build without ORCA_BO_LIVE it says the screen is a test
// build's and sends nothing to Brawl Online.
std::string_view Note();
// The line at the bottom that says how to use the screen: the pad's buttons, and the keyboard's
// keys too once `keyboard` (TextEntry::KeyboardSeen).
std::string_view Hint(bool keyboard);

// Moving the focus over items drawn in rows. `row_of[i]` is item i's row, the items in reading
// order (rows ascending). Up and Down go to the row above or below, wrapping, and land on its first
// item; Left and Right move along the focused item's row and stop at its ends. Returns the new focus.
enum class Move
{
  Up,
  Down,
  Left,
  Right,
};
int MoveFocus(const std::vector<int>& row_of, int focus, Move move);

// Whether online play runs or is starting: another player is in this game or on the way in, a room
// match is live, this game is joining someone's (Online::OthersPlaying), or Dolphin netplay runs. A
// room this game hosts alone does not count. Any thread.
bool OnlineActive();
// Tests: replaces OnlineActive's answer with `check`'s (nullptr restores it).
void SetOnlineCheckForTest(bool (*check)());
}  // namespace Orca::UX::BrawlOnlineSignIn
