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
// and no name, NameTags.h; 8: a ranked set's character order, its state in the match block and its
// input gate masks, CharOrder.h): the session hashes it into the compatibility key as `ux=<n>`, so
// two builds that would write different bytes never meet in a room.
constexpr int kCompatVersion = 8;

// Once, before the game boots in a session: registers the local pad source, the overlay, and the
// session's plug-in, frame and stall callbacks, and puts kCompatVersion in the compatibility key.
void Init();
// At exit, before the UI layer and logging shut down: stops the stream thread, unhooks the overlay.
void Shutdown();
}  // namespace Orca::UX
