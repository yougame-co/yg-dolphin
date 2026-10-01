// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <optional>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Session.h"
#include "Core/Orca/Session/YouGameRoom.h"

// Playing a friend over YouGame: the room this process plays in, for the rollback core that runs
// the session at each frame boundary. One room per process; the room's I/O runs on its own thread.
namespace Orca::Online
{
// A session that plays a friend online: ORCA_SESSION=1 plus the desktop app's bridge
// (YOUGAME_BRIDGE / YOUGAME_TOKEN), or ORCA_TEST_DEV_GAME for tests.
bool Enabled();

// Drop-in play. The room this process plays in starts at the first frame boundary (Start) and
// runs on its own thread: the host keeps playing while friends come and go.
void Start();
// Whether this Orca joins someone else's game (ORCA_JOIN=1, or the desktop app opened an invite),
// once known; nullopt before, or if the room ended first.
std::optional<bool> Joining();
// The room's transport (valid from Start until Shutdown, which must run on the emulator thread
// after the session stopped using it), this Orca's seat (its room slot, -1 before it is in), what
// other players did, and whether the room is gone.
Net::Transport* Transport();
int Seat();
std::vector<Net::PeerEvent> TakePeerEvents();
bool RoomEnded();
// Host: offer a joining seat the keyframe.
void OfferKeyframe(int seat, const Net::KeyframeInfo& info);

// The YouGame app's "prepare-join" (embed mode, ORCA.md "Embedding"): the host invited a friend who
// will drop into the running game. Starts a keyframe at the next frame boundary, while the friend
// accepts. Any thread (embed mode calls it on the main thread); never blocks.
void PrepareJoin();
// True once after a PrepareJoin (the emulator thread takes it at a frame boundary).
bool TakePrepareJoin();

// The latest round trip to the room server, in milliseconds (-1 before the first). `sequence`, if
// given, changes with each new measurement: feed Session::OnRoundTrip only when it changed.
int RoundTripMs(u32* sequence = nullptr);
// What the room is doing or why it ended, for the window title or log.
std::string StatusLine();

// Reports why the session ended to the desktop app (Orca::Status): a desync, the room's reason,
// or the session's own error. Nothing for a normal leave.
void ReportSessionEnd(const std::string& session_error);

// The friend stopped answering mid-match: reports peer_left with `sentence`, and tells the friend
// on the way out that the connection stalled (for its own report, if its game wakes up).
void ReportPeerStalled(std::string_view sentence);

// Leaves the room (reporting a running match void) and stops its thread. Call when emulation stops.
void Shutdown();

// The lobby compatibility key: players whose keys differ can't share a room. Built from the Orca
// build, the game profile and data files, every forced setting that two machines must share, the
// disc and the seeded save. Not the host CPU: Mac and PC players meet.
std::string CompatibilityKey();
}  // namespace Orca::Online
