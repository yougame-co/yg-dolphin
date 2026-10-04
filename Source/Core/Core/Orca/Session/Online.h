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
// The room for this process from now on, after Shutdown: a friend's (`code`, joining), or a new one
// of this player's own (empty code). Never asks the app for an invite. A joiner tells the host its
// player's own controls as the in-game UX last read them (Orca::Events::OwnControls).
void StartRoom(const std::string& code, bool joining);
// The room's code, once known ("" before, or with no room).
std::string Code();
// The room roster's name for a seat ("" when unknown or no room).
std::string SeatName(int seat);

// The app's "join <code>" and "leave" (embed mode; any thread, never blocks). The emulator thread
// takes them at a frame boundary.
void RequestJoin(const std::string& code);
std::optional<std::string> TakeJoinRequest();
void RequestLeave();
bool TakeLeaveRequest();
// The app's "host <code>" (embed mode, with the app's "host"; any thread, never blocks): host the
// room the queue matched this game into (ORCA.md "Matchmaking"). Taken at a frame boundary.
void RequestHost(const std::string& code);
std::optional<std::string> TakeHostRequest();

// ---- Matchmade rooms (ORCA.md "Matchmaking"; YouGameRoom's) ----
// The room's queue: "private" (a friends room, or no room), "casual" or "ranked".
std::string RoomQueue();
// A room match is running whose result isn't in yet.
bool MatchLive();
// Host: the opponent plays in this game (past its catch-up), or no longer does.
void SetOpponentPlugged(bool plugged);
// A game's result, for the room's match.
void ReportGame(const Net::GameReport& report);
// The games went apart: the room's match in progress is void.
void ReportDesync();
// A ranked set whose opponent stopped sending inputs (15 s stalled, or silent): this player claims
// the set (YouGameRoom::ReportStall). Nothing outside a ranked room's live match.
void ReportStall();
// The app's "delay auto|1..6" (embed mode; any thread, never blocks): this player's input delay
// from now on, in frames, or nullopt for the adaptive one. It stays for every session after, and
// counts only while the app's caps include "delay" (Rollback::OnlineMatch applies it at each
// frame boundary, and to a new session from its first frame).
void SetChosenDelay(std::optional<int> frames);
std::optional<int> ChosenDelay();
// The app's "direct on|off" (embed mode; any thread, never blocks): whether this player allows
// direct links to the other players (ORCA.md "Direct links"). On until the app says; it stays for
// every room after, and counts only while the app's caps include "direct". The room turns its
// links off or on at its next tick (YouGameRoom).
void SetDirectWanted(bool on);
bool DirectWanted();
// Whether this Orca joins someone else's game (ORCA_JOIN=1, or the desktop app opened an invite),
// once known; nullopt before, or if the room ended first.
std::optional<bool> Joining();
// The room's transport (valid from Start until Shutdown, which must run on the emulator thread
// after the session stopped using it), this Orca's seat (its room slot, -1 before it is in), what
// other players did, and whether the room is gone.
Net::Transport* Transport();
int Seat();
std::vector<Net::PeerEvent> TakePeerEvents();
// Whether those events, not taken yet, include the game's host leaving the room (any thread).
bool HostLeftPending();
bool RoomEnded();
// This Orca is in its room: the welcome came and the room hasn't ended.
bool InRoom();
// A fresh ticket for the room and the keyframe store's address (YouGameRoom::FreshTicket).
bool FreshTicket(bool fresh, std::string* ticket, std::string* store_url, std::string* error);
// Host: tell a friend it was unplugged and why ("desync", "stalled", "network").
void DropPeer(int seat, const std::string& reason);
// Host: whether it holds the joins that wait for a keyframe (YouGameRoom::HoldJoins).
void HoldJoins(bool holding);
// Whether a session error means the games went apart (inputs or a checksum that disagree, or a
// correction that came too late to apply).
bool IsDesync(const std::string& session_error);
// Whether a session error is a rollback this machine couldn't make ("No snapshot to roll back",
// "exceeds the limit"): an engine fault the player survives, reported in its own words.
bool IsRollbackFailure(const std::string& session_error);
// Host: every port's values so far and their version, to every player in the game
// (YouGameRoom::BroadcastNames).
void BroadcastNames(const std::vector<Net::KeyframeInfo::Name>& names, int version);
// Leaves the room like Shutdown, but on a thread of its own: the goodbye and the socket's close
// take up to a second or two, which the emulator thread must not wait out mid-game.
void ShutdownInBackground();
// Rooms ShutdownInBackground is still leaving (their page mirror may still post until then).
bool RoomsLeaving();
// Host: offer a joining seat the keyframe.
void OfferKeyframe(int seat, const Net::KeyframeInfo& info);

// The YouGame app's "prepare-join" (embed mode, ORCA.md "Embedding"): the host invited a friend who
// will drop into the running game. Starts a keyframe at the next frame boundary, while the friend
// accepts. Any thread (embed mode calls it on the main thread); never blocks.
void PrepareJoin();
// True once after a PrepareJoin (the emulator thread takes it at a frame boundary).
bool TakePrepareJoin();

// Drop-in work the next frame boundary will take: a friend arriving in the room (not taken yet) or
// a PrepareJoin while the room is up, the app's join or leave once its caps allow them. A solo
// pause ends for it (Rollback::OnlineMatch::SoloPauseAllowed). Any thread; never blocks for long.
bool DropInPending();

// The latest round trip to the room server, in milliseconds (-1 before the first). `sequence`, if
// given, changes with each new measurement: feed Session::OnRoundTrip only when it changed.
int RoundTripMs(u32* sequence = nullptr);
// The room's latest round trips, oldest first, for a session that starts now; `sequence` as above,
// for the newest (unchanged with no room).
std::vector<int> RecentRoundTrips(u32* sequence);
// How the other players' inputs reach this one: the relay, or direct links (Net::DirectLink).
Net::DirectSummary Direct();
// Why the room ended, as an Orca::Status code ("" while it runs, or when it gave none).
std::string RoomErrorCode();
// The room ended because someone else holds it: a hosting Orca that wasn't its host, or whose
// socket the server replaced with another run's (YouGameRoom's "taken"; RoomErrorCode says
// "network").
bool RoomTaken();
// What the room is doing or why it ended, for the window title or log.
std::string StatusLine();

// Reports why the session ended to the desktop app (Orca::Status): a desync, the room's reason,
// or the session's own error. Nothing for a normal leave. False when the game can't go on solo
// (this machine's emulation failed: an "internal" error, always terminal).
bool ReportSessionEnd(const std::string& session_error);

// Why this Orca leaves its room, for its bye (the others' "friend-left <why>", the room's lobby):
// "mismatch" (a queue room's header it wouldn't play), "no-show" (it never picked in time). Any
// thread.
void SetLeaveReason(const std::string& reason);

// The friend stopped answering mid-match: reports peer_left with `sentence`, and tells the friend
// on the way out that the connection stalled (for its own report, if its game wakes up).
void ReportPeerStalled(std::string_view sentence);

// Tests only (ORCA_TEST_COMMANDS' "test-drop-room"): the room's connection drops as a network
// failure would.
void TestDropRoom();
// Tests only (ORCA_TEST_COMMANDS' "test-direct off|on|in|out|rebuild"): direct links drop every
// datagram, or only incoming or outgoing ones, or none again; or every link is built anew.
void TestDirect(const std::string& command);

// Leaves the room (reporting a running match void) and stops its thread, then waits (2 s at most)
// for rooms ShutdownInBackground is still leaving. Call when emulation stops.
void Shutdown();

// The lobby compatibility key: players whose keys differ can't share a room. Built from the Orca
// build, the game profile and data files, every forced setting that two machines must share, the
// disc and the seeded save. Not the host CPU: Mac and PC players meet.
std::string CompatibilityKey();
}  // namespace Orca::Online
