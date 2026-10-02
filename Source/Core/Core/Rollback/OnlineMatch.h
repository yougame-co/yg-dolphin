// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Online drop-in play (Orca::Online: friends over a YouGame room), run at the frame boundary. The
// host plays solo from its boot; a friend who joins loads the host's keyframe, catches up through
// the host's inputs since, and plugs into the next port, from where a rollback session over the
// room's transport runs both games (RingPort is the emulator side). See ORCA.md, "Drop-in".

#pragma once

#include <chrono>
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
// `on_frame(frame, resimulating, ports, alone)` runs first at every boundary
// (Orca::Events::NotifyFrame), before anything is saved there; `ports` are the controllers plugged
// in at that frame; `alone`: this game plays alone with nothing of drop-in under way (as the last
// boundary left it: SoloPauseAllowed's state; false on re-runs).
using FrameHook = std::function<void(int frame, bool resimulating,
                                     const std::vector<Orca::Events::PortInfo>& ports, bool alone)>;
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

// The player's pause in a session (the app's "pause", Core::SetState). A pause may stop only this
// player's game: allowed in a session without a room (tests), and online while the last frame
// boundary left this game alone with nothing of drop-in under way (no friend plugged in or on the
// way, not joining or catching up, no keyframe wanted or being made) and nothing waits for the
// next one (Orca::Online::DropInPending: a friend arriving, an invite, a join or a leave).
// Any thread.
bool SoloPauseAllowed();
// Core::SetState(Paused) in a session: whether the pause may go ahead (SoloPauseAllowed). From a
// yes until EndPause, boundaries take no friend in, so one that still runs while the CPU thread
// winds down never starts a session it can't step (a step gives up while the CPU is stepping).
bool BeginPause();
// Core::SetState(Running): the pause, if any, is over.
void EndPause();
// Between a BeginPause that said yes and EndPause: the core is paused by the player's pause (not
// still stepping at boot, nor stopped by an HLE reload or a panic).
bool Pausing();
// Host time spent paused since the last call. The stats line leaves it out of its interval, so
// its fps counts running time only.
std::chrono::nanoseconds TakePausedTime();
}  // namespace Rollback::OnlineMatch
