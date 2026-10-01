// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// An online match (Orca::Online: a friend over a YouGame room), run at the frame boundary: the
// rollback session over the room's transport, with RingPort as the emulator side.

#pragma once

#include <functional>
#include <optional>

#include "Core/Orca/Session/Session.h"

namespace Core
{
class System;
}

namespace Rollback::OnlineMatch
{
// One frame boundary. The first call joins the room and blocks until the match starts (or
// emulation stops). `local_pad(seat)` gives this player's pad for the given seat. Returns the
// frame a rollback rewound to, if one started here (the state is now the start of that frame).
std::optional<int> OnBoundary(Core::System& system,
                              const std::function<Orca::Net::Pad(int local_seat)>& local_pad);

// True while a match's session runs.
bool Active();
// Logs the session's stats ("Online match <when>: ...").
void LogStats(const char* when);
// Logs the session's stats (and why it ended), ends the session and leaves the room. Safe to call
// more than once; called by itself when emulation stops or the session ends.
void End(const char* reason);
}  // namespace Rollback::OnlineMatch
