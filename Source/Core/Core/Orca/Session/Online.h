// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Session.h"

// Playing a friend over YouGame: the room this process plays in, for the rollback core that runs
// the session at each frame boundary. One room per process; the room's I/O runs on its own thread.
namespace Orca::Online
{
// A session that plays a friend online: ORCA_SESSION=1 plus the desktop app's bridge
// (YOUGAME_BRIDGE / YOUGAME_TOKEN), or ORCA_TEST_DEV_GAME for tests.
bool Enabled();

// Joins the room (ORCA_ROOM, else the app's invite, else a new code), blocks until the match
// starts and fills in config's seats and local seat. Null when the room ended (StatusLine says
// why) or `stopping` returned true. The transport stays valid until Shutdown, which must run on the
// emulator thread after the session stopped using it.
Net::Transport* WaitForMatch(Net::Config* config, const std::function<bool()>& stopping);

// The match's seed (valid once WaitForMatch returned a transport).
u32 Seed();
// The latest round trip to the room server, in milliseconds (-1 before the first). `sequence`, if
// given, changes with each new measurement: feed Session::OnRoundTrip only when it changed.
int RoundTripMs(u32* sequence = nullptr);
// What the room is doing or why it ended, for the window title or log.
std::string StatusLine();

// Leaves the room (reporting a running match void) and stops its thread. Call when emulation stops.
void Shutdown();

// The lobby compatibility key: players whose keys differ can't share a room. Built from the Orca
// build, the host CPU architecture (until cross-architecture determinism is proven), the game
// profile, every forced setting that two machines must share, the disc and the seeded save.
std::string CompatibilityKey();
}  // namespace Orca::Online
