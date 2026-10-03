// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <picojson.h>

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
  // A joiner's own controls (Orca::Events::OwnControls), told to the host in its hello: the host
  // puts them in the joiner's port (KeyframeInfo::Name::controls). Empty: none.
  std::vector<u8> controls;
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
  // The host's values for the game's ports (who plays each, and with which controls), each from
  // the frame it applies: the joiner names ports exactly as the host does (Orca::Events::PortInfo),
  // never from its own view of the roster.
  struct Name
  {
    int seat = 0;
    int from = 0;
    std::string name;
    // The player's own controls (Orca::Events::PortInfo::controls; at most MAX_CONTROLS bytes,
    // empty: none).
    std::vector<u8> controls;
    bool operator==(const Name&) const = default;
  };
  std::vector<Name> names;
  // The host's count of changes to `names` (Session::RequireValues): a player acknowledges the
  // newest it holds, and the host plugs a newcomer in only once every player holds its values.
  int names_version = 0;
};

// The most bytes a port's controls may have on the wire (UX/NameTags.h's profile is 46).
constexpr size_t MAX_CONTROLS = 64;

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
  // Arrived: the joiner's own controls from its hello (RoomOptions::controls; empty: none).
  std::vector<u8> controls;
  KeyframeInfo keyframe;
  // Left: why it said it was going ("desync" or "stalled", from its bye; empty: it just left).
  std::string reason;
};

// How the other players' inputs reach this one (DirectLink.h): the worst of them.
struct DirectSummary
{
  // 0 the relay only, 1 a direct link, 2 a direct link through TURN (`orca stats` "tx").
  int transport = 0;
  // Round trip on that link, in milliseconds (-1: relay, or not measured yet).
  int rtt_ms = -1;
  // For logs: "direct host/host 1 ms", "TURN relay/host 38 ms", "relay".
  std::string line = "relay";
};

// A game's result as this Orca read it (UX/Results.h), for a matchmade room's report.
struct GameReport
{
  enum class Kind
  {
    Win,
    Draw,
    Void,
  };
  Kind kind = Kind::Void;
  // Win: the winner's seat (its room slot).
  int winner_seat = -1;
  // Void: why ("nocontest", "ports").
  std::string why;
  // Win and Draw: {"f":..,"st":..,"c":[..],"s":[..],"to":..}, ports in order.
  std::string detail;
  // A ranked set's game from the match block (UX/RankedSet.h): its fight's first frame (the
  // report's id, "g<start>", the same on both machines; -1: the beta's "g<n>"), its number in the
  // set, and for the game that ended the set, how (1 won by `set_winner_seat`, 2 void).
  int start = -1;
  int number = 0;
  int set_done = 0;
  int set_winner_seat = -1;
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
  // Whether those events, not taken yet, include a friend arriving (Kind::Arrived on a seat after
  // the host's, from a player that isn't the game's host).
  bool ArrivalPending() const;
  // Host: offer `seat`'s player the keyframe (sent every second until its first packet arrives).
  void OfferKeyframe(int seat, const KeyframeInfo& info);
  // Host: every port's values so far (KeyframeInfo::names) and their version, to every player in
  // the game.
  void BroadcastNames(const std::vector<KeyframeInfo::Name>& names, int version);
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
  // Once Ended: the desktop app's error code for why (Orca::Status), empty for a normal end. One
  // code is the game's own: "taken", a hosting Orca that found someone else hosting its room (it
  // opens another, and reports that as "network").
  std::string ErrorCode() const;
  // The latest round trip to the room server and back, in milliseconds (-1 before the first).
  // `sequence`, if given, receives a count that changes with each new measurement: feed
  // Session::OnRoundTrip only when it changed since the last call.
  int RoundTripMs(u32* sequence = nullptr) const;
  // The latest RTT_SAMPLES round trips, oldest first, for a session that starts now (it covers the
  // link from its first frame), and in `sequence` the count RoundTripMs gives with the newest.
  std::vector<int> RecentRoundTrips(u32* sequence) const;
  // The direct links to the other players (refreshed a few times a second).
  DirectSummary Direct() const;
  // Tests only: `test-direct off|on|in|out|rebuild` (DirectLink::Test).
  void TestDirect(const std::string& command);

  // ---- Matchmade rooms (ORCA.md "Matchmaking") ----
  // The room's queue from its welcome: "private" (a friends room, and until the welcome),
  // "casual" or "ranked".
  std::string Queue() const;
  // A room match is running whose result isn't in yet (a ranked set until its result).
  bool MatchLive() const;
  // Host: the opponent is plugged in past its catch-up ("friend-joined"), so the room's match may
  // begin; false once it unplugs.
  void SetOpponentPlugged(bool plugged);
  // A game's result, reported to the room's match once it has one (held 60 s at most).
  void ReportGame(GameReport report);
  // The games went apart: the match in progress is reported void (a ranked set, or a casual game).
  void ReportDesync();

  void Send(const Packet& packet) override;
  std::vector<Packet> Receive() override;
  bool Connected() const override;

  // Leaves the room (reporting the match void if one is running).
  void Leave();
  // Why this player is about to leave, told to the friend first so both report the same thing
  // (an Orca::Status code; only "desync" is passed on).
  void SetLeaveReason(std::string reason);
  // Tests only: the connection drops as a network failure would (no goodbye, and the server holds
  // the seat); the room ends with "network".
  void TestDropConnection();

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// A ranked set's tally (ORCA.md "Matchmaking"; exposed for tests): the room's game verdicts in
// order, and the set's end once one player has won SET_WINS games, or SET_GAMES games passed
// without that (void, "limit"). Both players' Orcas see the same verdicts, so both end the set the
// same way.
class SetTally
{
public:
  static constexpr int SET_WINS = 2;
  static constexpr int SET_GAMES = 7;
  struct End
  {
    // The set's winner (a participant id); empty: the set is void.
    std::string winner;
    std::string why;
  };
  // One game's verdict: its winner's participant id, or "" for a draw or a void game. The set's
  // end, once (nothing after it).
  std::optional<End> Game(const std::string& winner);
  int Wins(const std::string& id) const;
  void Reset();

private:
  std::map<std::string, int> m_wins;
  int m_games = 0;
  bool m_ended = false;
};

// Packets as room messages (exposed for tests).
std::string EncodePacket(const Packet& packet);
bool DecodePacket(const std::string& json, Packet* packet);
// Port values in the room's "nm" and "kf" messages (exposed for tests): each [seat, from, name]
// and, for a port with controls, their hex; at most 16 entries are read, and controls longer than
// MAX_CONTROLS bytes, or not hex, are none.
picojson::value NamesToJson(const std::vector<KeyframeInfo::Name>& names);
std::vector<KeyframeInfo::Name> NamesFromJson(const picojson::object& message);
// A packet from a direct link to the player at `slot`: as through the relay, only for that player's
// own seat, and never with drop-in history (the relay alone carries it, in order).
bool DecodeDirectPacket(const std::string& payload, int slot, Packet* packet);
// YouGame's switch for direct links in a ticket reply: its top-level `direct`, false turning them
// off. Absent or not a boolean (an older site): on.
bool TicketAllowsDirect(const picojson::object& reply);
// What to tell the player when the room refused the connection (WebSocket::Connect's error), and
// the desktop app's error code for it.
std::string RefusalReason(const std::string& connect_error);
std::string RefusalCode(const std::string& connect_error);
// Queues `packet` in place of one not sent yet, keeping the older one's earlier inputs and checksum.
void CoalescePacket(std::optional<Packet>* outgoing, const Packet& packet);
}  // namespace Orca::Net
