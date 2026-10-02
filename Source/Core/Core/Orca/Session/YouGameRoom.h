// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Session.h"

// A private YouGame room carrying an Orca drop-in session: the host plays from its boot, friends
// join by room code (up to four players), each friend's seat is its room slot (seat 0, the host,
// is controller port 1), and session packets travel as relayed room messages. Tickets come from the YouGame desktop app's loopback bridge
// (YOUGAME_BRIDGE / YOUGAME_TOKEN), or, for tests only, straight from the site as an anonymous dev
// game (ORCA_TEST_DEV_GAME: its rooms belong to a separate dev game, so they never meet real
// players). All network work runs on one thread.
namespace Orca::Net
{
struct RoomOptions
{
  // 6-12 characters [a-z0-9]; empty: the desktop app's invite (hello.room), else a new code.
  std::string room_code;
  // Lobby compatibility key: players with different keys can't share a room.
  std::string compatibility;
  std::string player_name = "Player";
  // The room's mode: one per game, e.g. "orca-rsbe01". Both players must use the same.
  std::string mode = "orca-test";
  // This Orca joins a friend's running game (ORCA_JOIN=1). Also true when the app opened an invite.
  bool joining = false;
  // With no room_code, ask the desktop app whether the player opened an invite (only at launch: a
  // room opened later, after a leave, is always a new one).
  bool ask_app_for_room = true;
};

// Where the host's keyframe of its game is, for a joining friend (Orca::Net::KeyframeStore).
struct KeyframeInfo
{
  int frame = -1;
  std::string id;
  u64 size = 0;
  std::string hash;
  // The keyframe's AES-256-GCM key (64 hex), only ever in the room's `kf` message.
  std::string key;
  // The host's player names for the game's ports, each from the frame it applies: the joiner names
  // ports exactly as the host does (Orca::Events::PortInfo), never from its own view of the roster.
  struct Name
  {
    int seat = 0;
    int from = 0;
    std::string name;
  };
  std::vector<Name> names;
};

// Something another player in the room did.
struct PeerEvent
{
  enum class Kind
  {
    // It said hello with our compatibility key: a player of this game at `seat`.
    Arrived,
    // It left the room or its connection dropped.
    Left,
    // The host offered its keyframe (to a joiner).
    Keyframe,
    // The host's port names changed (a friend was taken in): `keyframe.names` only.
    Names,
  };
  Kind kind = Kind::Arrived;
  int seat = -1;
  // The player is the game's host (the Orca that started it).
  bool host = false;
  std::string name;
  KeyframeInfo keyframe;
  // Left: why it said it was going ("desync" or "stalled", from its bye; empty: it just left).
  std::string reason;
};

enum class RoomState
{
  Connecting,
  // In the room.
  Waiting,
  Ended,
};

class YouGameRoom final : public Transport
{
public:
  explicit YouGameRoom(RoomOptions options);
  ~YouGameRoom() override;

  RoomState GetState() const;
  // This Orca's room slot (its seat), once in the room (-1 before).
  int Seat() const;
  // Whether this Orca joins someone else's game, once the ticket is in (nullopt before, or if the
  // room ended first).
  std::optional<bool> Joining() const;
  // What other players did since the last call.
  std::vector<PeerEvent> TakePeerEvents();
  // Host: offer `seat`'s player the keyframe (sent every second until its first packet arrives).
  void OfferKeyframe(int seat, const KeyframeInfo& info);
  // Host: every port name so far (KeyframeInfo::names), to every player in the game.
  void BroadcastNames(const std::vector<KeyframeInfo::Name>& names);
  // Host: tell `seat`'s player it was unplugged and why ("desync", "stalled", "network"), so its
  // own report says the same. Its room ends with that code.
  void DropPeer(int seat, const std::string& reason);
  // A ticket for this room (with `fresh`, a newly minted one: the store refused the last), and the
  // keyframe store's address for it (".../api/orca/keyframes/<room>"). Any thread; minting blocks
  // on an HTTP request.
  bool FreshTicket(bool fresh, std::string* ticket, std::string* store_url, std::string* error);
  // The room code in use (for showing and sharing), once known.
  std::string Code() const;
  // The roster's name for a seat (this player's own included), "" when nobody holds it.
  std::string SeatName(int seat) const;
  // A line for the UI or log: what the room is doing, or why it ended.
  std::string Status() const;
  // Once Ended: the desktop app's error code for why (Orca::Status), empty for a normal end.
  std::string ErrorCode() const;
  // The latest round trip to the room server and back, in milliseconds (-1 before the first).
  // `sequence`, if given, receives a count that changes with each new measurement: feed
  // Session::OnRoundTrip only when it changed since the last call.
  int RoundTripMs(u32* sequence = nullptr) const;

  void Send(const Packet& packet) override;
  std::vector<Packet> Receive() override;
  bool Connected() const override;

  // Leaves the room (reporting the match void if one is running).
  void Leave();
  // Why this player is about to leave, told to the friend first so both report the same thing
  // (an Orca::Status code; only "desync" is passed on).
  void SetLeaveReason(std::string reason);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// Packets as room messages (exposed for tests).
std::string EncodePacket(const Packet& packet);
bool DecodePacket(const std::string& json, Packet* packet);
// What to tell the player when the room refused the connection (WebSocket::Connect's error), and
// the desktop app's error code for it.
std::string RefusalReason(const std::string& connect_error);
std::string RefusalCode(const std::string& connect_error);
// Queues `packet` in place of one not sent yet, keeping the older one's earlier inputs and checksum.
void CoalescePacket(std::optional<Packet>* outgoing, const Packet& packet);
}  // namespace Orca::Net
