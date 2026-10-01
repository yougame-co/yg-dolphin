// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Session.h"

// A private YouGame room carrying an Orca session: two friends join by room code, the host starts
// the match, seats follow the room's slot order (seat 0 = controller port 1), and session packets
// travel as relayed room messages. Tickets come from the YouGame desktop app's loopback bridge
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
};

enum class RoomState
{
  Connecting,
  // In the room, waiting for the other player (or for the host to start).
  Waiting,
  Playing,
  Ended,
};

class YouGameRoom final : public Transport
{
public:
  explicit YouGameRoom(RoomOptions options);
  ~YouGameRoom() override;

  RoomState GetState() const;
  // Valid once Playing.
  int Seat() const;
  u32 Seed() const;
  // The room code in use (for showing and sharing), once known.
  std::string Code() const;
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
