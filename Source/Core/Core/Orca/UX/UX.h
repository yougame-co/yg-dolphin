// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Orca's in-game UX for online play (docs: YouGame docs/ORCA_ONLINE_UX.md): the local controller
// from the YouGame app, the minimal overlay, the players' names in the game's own name tags.
namespace Orca::UX
{
// Bumped whenever anything here that writes emulated memory changes (the name tags; 2: the game
// patches, GamePatches.h, whose file is also hashed into the key; 3: a launcher profile's patches
// are applied too, PPLUS32.patches; 4: the main menu's text about going online, MenuText.h; 5: the
// menu's description box's copy of that text too; 6: a matchmade host's rules preset, Results.h;
// 7: each player's own controls in their name tag, and the port label for a player with controls
// and no name, NameTags.h; 8: the online rules for queue matches (OnlineRules.h, which replace 6)
// and the ranked set's stage, character and result flows (RankedPPlus.h, BrawlStages.h,
// CharOrder.h, RankedSet.h), with their input masks; 9: A on the character select only in the
// grid while the online rules lock, OnlineRules.h; 10: queue2's character select, ready timer and
// steered pick (Queue.h, the header's queue flags), Project+ booting to its main menu and a friend's
// drop-in from the menus (PPLUS32.patches, OnlineMenu.h FriendsMove), the character select's title
// (CssTitle.h), the casual stage pick and the later-game character order (RankedSteps.h,
// BrawlStages.h, RankedPPlus.h, CharOrder.h) with the gate's a_as and steer; 11, Orca 0.3.15: no
// Z on the results screen in any session, its replay save's dead end, OnlineRules.h SessionMasks;
// 12: the character select's A to pick, B to take the token back up and Start to lock in
// (OnlineRules.h CssBUnpicks, CharOrder.h, Queue.h; the raw latch under every header that locks)):
// the session hashes it into the compatibility key as `ux=<n>`, so two builds that would write
// different bytes (or mask different inputs) never meet in a room.
constexpr int kCompatVersion = 12;

// Once, before the game boots in a session: registers the local pad source, the overlay, and the
// session's plug-in, frame and stall callbacks, and puts kCompatVersion in the compatibility key.
void Init();
// At exit, before the UI layer and logging shut down: stops the stream thread, unhooks the overlay.
void Shutdown();
}  // namespace Orca::UX
