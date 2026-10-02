// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Orca's in-game UX for online play (docs: YouGame docs/ORCA_ONLINE_UX.md): the local controller
// from the YouGame app, the minimal overlay, the players' names in the game's own name tags.
namespace Orca::UX
{
// Bumped whenever anything here that writes emulated memory changes (the name tags): the session
// hashes it into the compatibility key as `ux=<n>`, so two builds that would write different bytes
// never meet in a room.
constexpr int kCompatVersion = 1;

// Once, before the game boots in a session: registers the local pad source and the session's
// plug-in callback. Outside a session it does nothing.
void Init();
// At exit, before the UI layer and logging shut down: stops the stream thread, unhooks the overlay.
void Shutdown();
}  // namespace Orca::UX
