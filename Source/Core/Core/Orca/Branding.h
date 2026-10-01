// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

// What a player sees of this program is called Orca. Upstream attribution and licence text stay
// as they are; only player-visible names change.
namespace Orca
{
// The window title in a session: "Orca — <game> — <room status>" (the status while there is one).
std::string WindowTitle();

// Text a player can see in a session (on-screen messages, alerts), with the upstream emulator's
// name replaced by Orca's. Outside a session the text is returned unchanged.
std::string BrandText(std::string_view text);
}  // namespace Orca
