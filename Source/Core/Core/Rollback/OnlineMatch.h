// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Online drop-in play (Orca::Online: friends over a YouGame room), run at the frame boundary. The
// host plays solo from its boot; a friend who joins loads the host's keyframe, catches up through
// the host's inputs since, and plugs into the next port, from where a rollback session over the
// room's transport runs both games (RingPort is the emulator side). See ORCA.md, "Drop-in".

#pragma once

#include <functional>
#include <optional>

#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Session.h"

namespace Core
{
class System;
}

namespace Rollback::OnlineMatch
{
// One frame boundary. The first call joins the room; a joiner blocks there until it has loaded the
// host's keyframe (or emulation stops). `local_pad(seat)` gives this player's pad for the given
// seat. Returns the frame the state was rewound to here (a rollback, or a joiner's keyframe): the
// state is now the start of that frame.
// `on_frame(frame, resimulating, ports)` runs first at every boundary (Orca::Events::NotifyFrame),
// before anything is saved there; `ports` are the controllers plugged in at that frame.
using FrameHook = std::function<void(int frame, bool resimulating,
                                     const std::vector<Orca::Events::PortInfo>& ports)>;
std::optional<int> OnBoundary(Core::System& system,
                              const std::function<Orca::Net::Pad(int local_seat)>& local_pad,
                              const FrameHook& on_frame);

// True while this process plays online (solo in its room, or with friends).
bool Active();
// Logs the session's stats ("Online match <when>: ...").
void LogStats(const char* when);
// Logs the session's stats (and why it ended), ends the session and leaves the room. Safe to call
// more than once; called by itself when emulation stops or the session ends.
void End(const char* reason);
}  // namespace Rollback::OnlineMatch
