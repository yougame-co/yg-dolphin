// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "Core/Orca/UX/MenuEvents.h"

// The Brawl Online entry in the game's Online menu (Games/Brawl/OnlineMenu.h reads it). Its page
// opens the Brawl Online sign-in screen (UX/BrawlOnlineSignIn.h) the first time on this PC; once a
// link is kept it opens nothing. Each of its two rows (Casual, With Friends) leaves the main menu
// for the local character select, where a sign-in-style panel says a test build sends nothing to
// Brawl Online. No network path: nothing here talks to anything. Host-only: nothing here reads or
// writes emulated state.
namespace Orca::UX::BrawlOnlineEntry
{
// The YouGame player the sign-in is for: ORCA_YOUGAME_PLAYER from the app, or LOCAL_PLAYER when the
// app passes none.
constexpr std::string_view LOCAL_PLAYER = "this-pc";
std::string PlayerId();

// The Brawl Online page just opened (first runs): the sign-in screen, unless this PC already keeps
// the player's link. CPU thread.
void PageOpened();

// The row whose character select shows now (BrawlOnlineCasual or BrawlOnlineFriends), or none.
// Written by the frame hook on first runs; any thread reads.
void SetRow(std::optional<OnlinePick> row);
std::optional<OnlinePick> Row();

// The panel's title for a row ("Brawl Online: Casual", "Brawl Online: With Friends"); "" for any
// other pick. Pure.
std::string_view RowTitle(OnlinePick row);
// The line under it (BrawlOnlineSignIn::Note), then how to leave.
constexpr std::string_view BACK_HINT = "Press B to go back.";

// The overlay's draw (video thread, inside the ImGui frame). Nothing without a row, or while the
// sign-in screen is open.
void Draw();
}  // namespace Orca::UX::BrawlOnlineEntry
