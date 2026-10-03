// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

// Direct links between the Orcas in a room: one UDP path per pair of players, beside the room's
// relay, so an input goes straight to the other machine instead of through Cloudflare and the
// room's Durable Object (about 15 ms each way more; on one LAN a 40-50 ms round trip where a direct
// one takes 1 ms).
//
// Slippi plays over plain UDP between the two players (ENet, inputs unsequenced and repeated until
// acknowledged), hole-punched with its matchmaking server's help, with no fallback when that fails
// (ORCA.md, "Direct links"). Orca does the same data path, but connects with ICE (libjuice): host
// candidates on every interface (IPv4 and IPv6), the public address from STUN, and Cloudflare TURN
// from the room ticket when nothing else gets through. Signalling rides the room as peer messages.
// Every datagram is sealed with ChaCha20-Poly1305 under keys both players contribute to, so only
// the two Orcas of a pair can make one. The relay keeps carrying every packet too: whichever copy
// arrives first counts (the session takes duplicates and reordering, Session::ReceivePackets), so a
// link that fails, or one that never opens, costs nothing but the time the relay takes.
namespace Orca::Net
{
// The ICE servers from a room ticket's `ice` (YouGame's web/src/lib/turn.ts): one STUN server, and
// one TURN server over UDP (TURN over TCP or TLS: the room's own relay already covers networks
// that block UDP).
struct IceConfig
{
  std::string stun_host = "stun.cloudflare.com";
  u16 stun_port = 3478;
  // Empty: no TURN.
  std::string turn_host;
  u16 turn_port = 3478;
  std::string turn_user;
  std::string turn_pass;
  bool operator==(const IceConfig&) const = default;
};

// The ticket reply's `ice` array: [{urls: string or [string], username?, credential?}]. Takes the
// first stun: URL, and the first turn: URL over UDP (no transport, or transport=udp), preferring
// port 3478 to 53. False when `ice` isn't an array (the config keeps its defaults).
bool ParseIce(const picojson::value& ice, IceConfig* out);

// How a peer's inputs reach this machine.
enum class LinkKind
{
  Relay = 0,   // only through the room
  Direct = 1,  // a direct path (host, server- or peer-reflexive candidates)
  Turn = 2,    // a direct link through a TURN server
};

struct DirectStats
{
  LinkKind kind = LinkKind::Relay;
  // The median of recent round trips on the link, from the datagrams themselves (-1: none yet).
  int rtt_ms = -1;
  // Candidate types of the selected pair, local/remote ("host/host"; "" before one is selected).
  std::string pair;
  int generation = 0;
  u64 sent = 0;
  u64 received = 0;
  // Datagrams that failed authentication, replays of ones already taken, and authentic packets
  // dropped for coming faster than an honest peer sends them.
  u64 auth_drops = 0;
  u64 replays = 0;
  u64 floods = 0;
  // The longest a send took on the caller's thread, in microseconds.
  int max_send_us = 0;
  // Links built for this peer, the latest reason ("new", "failed", "silent", "upgrade", ...).
  int builds = 0;
  std::string reason;
};

struct DirectOptions
{
  // This Orca's connection id in the room, and the room code (bound into the link keys).
  std::string me;
  std::string room;
  IceConfig ice;
  // When `ice` was minted, and how to mint fresh credentials (blocking; on the link's own thread):
  // TURN credentials live 30 minutes, and a friend may arrive long after the room opened.
  std::chrono::steady_clock::time_point ice_minted = std::chrono::steady_clock::now();
  std::function<std::optional<IceConfig>()> fresh_ice;

  // Test knobs (ORCA_DIRECT_*): advertise and accept only relay candidates ("turn") or nothing on
  // a host interface ("srflx"); also offer loopback candidates (two Orcas on one machine); drop
  // this many percent of the datagrams sent; start with every datagram dropped (as `Test("off")`).
  std::string force;
  bool loopback = false;
  int loss_percent = 0;
  bool blocked = false;
  // ORCA_TEST_NET_DELAY_MS / ORCA_TEST_NET_JITTER_MS, as YouGameRoom applies them to the relay:
  // every datagram waits this long (plus 0..jitter) on the way out and again on the way in.
  std::optional<int> net_delay_ms;
  int net_jitter_ms = 0;
};

// Options from the environment: nullopt when ORCA_DIRECT=0 (relay only).
std::optional<DirectOptions> DirectOptionsFromEnv();

class DirectLink
{
public:
  // A received packet: from which connection (and its seat), and the payload as sent.
  struct Received
  {
    std::string from;
    int slot = -1;
    std::string payload;
  };

  DirectLink();
  ~DirectLink();
  DirectLink(const DirectLink&) = delete;
  DirectLink& operator=(const DirectLink&) = delete;

  // ---- The room's I/O thread (none of these block) ----

  // Starts the link thread. It binds a throwaway UDP socket at once, so an operating system that
  // asks the player whether to let Orca use the network (the Windows Defender Firewall) asks now,
  // in the lobby, rather than when a friend arrives mid-game.
  void Start(DirectOptions options);
  // A player of this game (it said hello with our compatibility key and supports direct links).
  // The lower connection id of the pair offers; the other answers.
  void AddPeer(const std::string& id, int slot);
  // The player left, or its connection was replaced: its link goes, with its unread packets.
  void RemovePeer(const std::string& id);
  // A signalling message from a peer added before (`dl`, `dlc`, `dlr`).
  void OnSignal(const std::string& id, const picojson::object& message);
  // Messages for peers, to send through the room (to, message).
  std::vector<std::pair<std::string, picojson::object>> TakeSignals();
  // The peer's packets still arrive through the relay (a silent link is then worth rebuilding).
  void OnRelayPacket(const std::string& id);
  // Fresh ICE servers (a new ticket), for links built from now on.
  void SetIce(const IceConfig& ice);

  // ---- Any thread ----

  // To every peer with an open link, at once on the caller's thread (it never takes the room's
  // locks). At most MAX_PAYLOAD bytes.
  void Send(const std::string& payload);
  // Packets received since the last call.
  std::vector<Received> Receive();
  // How each peer's link is doing (nullopt: not a peer).
  std::optional<DirectStats> Stats(const std::string& id) const;
  // Tests (`test-direct off|on|in|out|rebuild`): drop every datagram, or only incoming or outgoing
  // ones, or none; rebuild every link now.
  void Test(const std::string& command);
  // Closes every link and stops the thread (also in the destructor). Send and Receive do nothing
  // after.
  void Stop();

  // The largest payload a datagram carries: 1200 bytes on the wire at most (IPv6's minimum MTU of
  // 1280 less IP, UDP and a TURN channel's headers), less the header and the tag.
  static constexpr size_t MAX_DATAGRAM = 1200;
  static constexpr size_t HEADER_SIZE = 18;
  static constexpr size_t TAG_SIZE = 16;
  static constexpr size_t MAX_PAYLOAD = MAX_DATAGRAM - HEADER_SIZE - TAG_SIZE;

  struct Impl;

private:
  std::unique_ptr<Impl> m_impl;
};

// Exposed for tests: the keys of one link and its datagrams.
namespace DirectCrypto
{
using Key = std::array<u8, 32>;
// Each side's contribution and connection id; the room code; the link's generation. The first key
// seals what the lower id sends, the second what the higher id sends.
std::pair<Key, Key> DeriveKeys(const Key& low_half, const Key& high_half, const std::string& room,
                               u16 generation, const std::string& low_id,
                               const std::string& high_id);

// A datagram's header (DirectLink.cpp has the layout). `counter` is the nonce: from 1, never twice
// under one key. `echo` and `hold`: the newest counter received from the peer and how long ago, in
// 0.1 ms (the peer's round trip).
struct Header
{
  u8 kind = 1;
  u16 generation = 0;
  u64 counter = 0;
  u32 echo = 0;
  u16 hold = 0;
};
// The datagram (empty if the payload is too large or the counter 0).
std::vector<u8> Seal(const Key& key, const Header& header, std::string_view payload);
// The payload of a datagram sealed with `key`, its header in `header` (nullopt: not one).
std::optional<std::string> Open(const Key& key, const u8* data, size_t size, Header* header);
// The header without checking the datagram (false: not an Orca datagram).
bool PeekHeader(const u8* data, size_t size, Header* header);
// The counters taken, the newest and the 63 before it: anything older or taken already is a replay.
struct ReplayWindow
{
  u64 top = 0;
  u64 seen = 0;
  bool Fresh(u64 counter) const;
  void Take(u64 counter);
};
}  // namespace DirectCrypto
}  // namespace Orca::Net
