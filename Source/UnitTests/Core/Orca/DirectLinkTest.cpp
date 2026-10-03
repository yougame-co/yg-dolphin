// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <picojson.h>

#include "Core/Orca/Session/DirectLink.h"
#include "Core/Orca/Session/YouGameRoom.h"

using namespace Orca::Net;
using namespace std::chrono_literals;

namespace
{
DirectCrypto::Key KeyOf(u8 byte)
{
  DirectCrypto::Key key;
  key.fill(byte);
  return key;
}

picojson::value Json(const char* text)
{
  picojson::value value;
  EXPECT_TRUE(picojson::parse(value, text).empty()) << text;
  return value;
}

// Orcas in this process, each with its DirectLink, signalling through the test as the room carries
// it (peer messages, in order). No STUN, no TURN: nothing leaves this machine. Loopback candidates
// too, in case the host's LAN addresses refuse a send to themselves.
class Mesh
{
public:
  struct Node
  {
    std::string id;
    int slot;
    std::unique_ptr<DirectLink> link;
    std::vector<DirectLink::Received> received;
  };

  explicit Mesh(int count, const std::function<void(DirectOptions&, int)>& tweak = {})
  {
    for (int i = 0; i < count; ++i)
    {
      Node node{std::string(1, static_cast<char>('a' + i)) + "-connection", i,
                std::make_unique<DirectLink>(), {}};
      DirectOptions options;
      options.me = node.id;
      options.room = "testroom";
      options.ice.stun_host.clear();
      options.loopback = true;
      if (tweak)
        tweak(options, i);
      node.link->Start(std::move(options));
      m_nodes.push_back(std::move(node));
    }
    for (const Node& a : m_nodes)
    {
      for (const Node& b : m_nodes)
      {
        if (a.id != b.id)
          a.link->AddPeer(b.id, b.slot);
      }
    }
  }

  Node& At(int i) { return m_nodes[static_cast<size_t>(i)]; }

  // Signalling from each node to the others, and what each received.
  void Pump()
  {
    for (Node& from : m_nodes)
    {
      for (auto& [to, message] : from.link->TakeSignals())
      {
        m_signals.push_back(message);
        for (Node& node : m_nodes)
        {
          if (node.id == to)
            node.link->OnSignal(from.id, message);
        }
      }
      for (DirectLink::Received& r : from.link->Receive())
        from.received.push_back(std::move(r));
    }
  }

  bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds limit = 10s)
  {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until)
    {
      Pump();
      if (done())
        return true;
      std::this_thread::sleep_for(5ms);
    }
    Pump();
    return done();
  }

  // How node `i` sees node `j`'s link.
  DirectStats Stats(int i, int j) { return At(i).link->Stats(At(j).id).value_or(DirectStats{}); }
  bool Up(int i, int j) { return Stats(i, j).kind != LinkKind::Relay; }
  bool AllUp()
  {
    for (size_t i = 0; i < m_nodes.size(); ++i)
    {
      for (size_t j = 0; j < m_nodes.size(); ++j)
      {
        if (i != j && !Up(static_cast<int>(i), static_cast<int>(j)))
          return false;
      }
    }
    return true;
  }

  const std::vector<picojson::object>& Signals() const { return m_signals; }

private:
  std::vector<Node> m_nodes;
  std::vector<picojson::object> m_signals;
};

int Count(const std::vector<DirectLink::Received>& received, const std::string& payload)
{
  return static_cast<int>(std::count_if(received.begin(), received.end(),
                                        [&](const auto& r) { return r.payload == payload; }));
}
}  // namespace

TEST(OrcaDirectLink, ParsesTheTicketsIceServers)
{
  // As YouGame's ticket route hands out Cloudflare's (web/src/lib/turn.ts).
  IceConfig c;
  ASSERT_TRUE(ParseIce(Json(R"([{"urls":["stun:stun.cloudflare.com:3478",
      "turn:turn.cloudflare.com:3478?transport=udp","turn:turn.cloudflare.com:3478?transport=tcp",
      "turns:turn.cloudflare.com:5349?transport=tcp"],"username":"u1","credential":"p1"}])"),
                       &c));
  EXPECT_EQ(c.stun_host, "stun.cloudflare.com");
  EXPECT_EQ(c.stun_port, 3478);
  EXPECT_EQ(c.turn_host, "turn.cloudflare.com");
  EXPECT_EQ(c.turn_port, 3478);
  EXPECT_EQ(c.turn_user, "u1");
  EXPECT_EQ(c.turn_pass, "p1");

  // `urls` as one string; TURN on 53 when that is all there is.
  IceConfig d;
  ASSERT_TRUE(ParseIce(Json(R"([{"urls":"stun:s.example:19302"},
      {"urls":"turn:t.example:53?transport=udp","username":"x","credential":"y"}])"),
                       &d));
  EXPECT_EQ(d.stun_host, "s.example");
  EXPECT_EQ(d.stun_port, 19302);
  EXPECT_EQ(d.turn_host, "t.example");
  EXPECT_EQ(d.turn_port, 53);

  // TURN over TCP or TLS only, or without credentials: no TURN (the room's relay covers that).
  IceConfig e;
  ASSERT_TRUE(ParseIce(Json(R"([{"urls":["turn:t.example:3478?transport=tcp",
      "turns:t.example:5349"],"username":"x","credential":"y"},{"urls":"turn:t.example"}])"),
                       &e));
  EXPECT_TRUE(e.turn_host.empty());
  EXPECT_EQ(e.stun_host, "stun.cloudflare.com");

  IceConfig f;
  EXPECT_FALSE(ParseIce(picojson::value(1.0), &f));
  EXPECT_FALSE(ParseIce(picojson::value(), &f));
}

TEST(OrcaDirectLink, KeysDifferPerDirectionGenerationRoomAndPair)
{
  const auto base = DirectCrypto::DeriveKeys(KeyOf(1), KeyOf(2), "room", 1, "a", "b");
  EXPECT_NE(base.first, base.second);
  EXPECT_EQ(DirectCrypto::DeriveKeys(KeyOf(1), KeyOf(2), "room", 1, "a", "b"), base);
  EXPECT_NE(DirectCrypto::DeriveKeys(KeyOf(1), KeyOf(2), "room", 2, "a", "b").first, base.first);
  EXPECT_NE(DirectCrypto::DeriveKeys(KeyOf(1), KeyOf(2), "room2", 1, "a", "b").first, base.first);
  EXPECT_NE(DirectCrypto::DeriveKeys(KeyOf(2), KeyOf(1), "room", 1, "a", "b").first, base.first);
  EXPECT_NE(DirectCrypto::DeriveKeys(KeyOf(1), KeyOf(2), "room", 1, "a", "c").first, base.first);
  EXPECT_NE(DirectCrypto::DeriveKeys(KeyOf(1), KeyOf(3), "room", 1, "a", "b").first, base.first);
}

TEST(OrcaDirectLink, DatagramsAuthenticateAndRefuseReplays)
{
  const DirectCrypto::Key key = KeyOf(7);
  DirectCrypto::Header header;
  header.kind = 1;
  header.generation = 3;
  header.counter = 9;
  header.echo = 5;
  header.hold = 12;
  const std::vector<u8> datagram = DirectCrypto::Seal(key, header, "inputs for frame 120");
  ASSERT_EQ(datagram.size(), DirectLink::HEADER_SIZE + 20 + DirectLink::TAG_SIZE);
  DirectCrypto::Header got;
  const auto payload = DirectCrypto::Open(key, datagram.data(), datagram.size(), &got);
  ASSERT_TRUE(payload);
  EXPECT_EQ(*payload, "inputs for frame 120");
  EXPECT_EQ(got.kind, 1);
  EXPECT_EQ(got.generation, 3);
  EXPECT_EQ(got.counter, 9u);
  EXPECT_EQ(got.echo, 5u);
  EXPECT_EQ(got.hold, 12);
  // Unreadable on the wire.
  const std::string clear = "inputs";
  EXPECT_EQ(std::search(datagram.begin(), datagram.end(), clear.begin(), clear.end()),
            datagram.end());
  // Another key (another pair, direction or generation): refused.
  EXPECT_FALSE(DirectCrypto::Open(KeyOf(8), datagram.data(), datagram.size(), &got));
  // Any byte changed, the header included: refused.
  for (size_t i = 0; i < datagram.size(); ++i)
  {
    std::vector<u8> tampered = datagram;
    tampered[i] ^= 0x10;
    EXPECT_FALSE(DirectCrypto::Open(key, tampered.data(), tampered.size(), &got)) << "byte " << i;
  }
  // Cut short, or empty.
  EXPECT_FALSE(DirectCrypto::Open(key, datagram.data(), datagram.size() - 1, &got));
  EXPECT_FALSE(DirectCrypto::Open(key, datagram.data(), 0, &got));
  // Counter 0 and payloads past one datagram are never sealed.
  header.counter = 0;
  EXPECT_TRUE(DirectCrypto::Seal(key, header, "x").empty());
  header.counter = 10;
  EXPECT_TRUE(DirectCrypto::Seal(key, header, std::string(DirectLink::MAX_PAYLOAD + 1, 'x')).empty());
  EXPECT_EQ(DirectCrypto::Seal(key, header, std::string(DirectLink::MAX_PAYLOAD, 'x')).size(),
            DirectLink::MAX_DATAGRAM);

  // The replay window: each counter once, out of order within the last 64.
  DirectCrypto::ReplayWindow window;
  EXPECT_FALSE(window.Fresh(0));
  EXPECT_TRUE(window.Fresh(1));
  window.Take(1);
  EXPECT_FALSE(window.Fresh(1));
  window.Take(5);
  EXPECT_TRUE(window.Fresh(3));
  window.Take(3);
  EXPECT_FALSE(window.Fresh(3));
  EXPECT_TRUE(window.Fresh(4));
  window.Take(100);
  EXPECT_FALSE(window.Fresh(100));
  EXPECT_FALSE(window.Fresh(36));
  EXPECT_TRUE(window.Fresh(37));
  EXPECT_TRUE(window.Fresh(99));
}

TEST(OrcaDirectLink, DirectPacketsOnlyForTheSendersSeat)
{
  Packet packet;
  packet.seat = 1;
  packet.sequence = 4;
  packet.first_frame = 10;
  packet.pads = {Pad{1, 2, 3, 4, 5, 6, 7, 8}};
  packet.current_frame = 11;
  Packet got;
  ASSERT_TRUE(DecodeDirectPacket(EncodePacket(packet), 1, &got));
  EXPECT_EQ(got.sequence, 4);
  EXPECT_EQ(got.pads, packet.pads);
  // A friend's link speaks for its own seat only.
  EXPECT_FALSE(DecodeDirectPacket(EncodePacket(packet), 2, &got));
  // History travels through the relay alone.
  packet.history_seat = 2;
  packet.history_first = 0;
  packet.history.resize(1);
  EXPECT_FALSE(DecodeDirectPacket(EncodePacket(packet), 1, &got));
  EXPECT_FALSE(DecodeDirectPacket("{not json", 1, &got));
}

// Two Orcas link over this machine: the lower id offers, the link comes up on host candidates,
// packets go both ways, the round trip is measured from the datagrams, and three rebuilds each
// bring a new generation back up with nothing refused.
TEST(OrcaDirectLink, TwoOrcasLinkAndRebuild)
{
  Mesh mesh(2);
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.AllUp(); })) << mesh.Stats(0, 1).reason;
  EXPECT_EQ(mesh.Stats(0, 1).kind, LinkKind::Direct);
  // A host or loopback path (libjuice may name it peer-reflexive), never a relayed one.
  EXPECT_EQ(mesh.Stats(0, 1).pair.find("relay"), std::string::npos) << mesh.Stats(0, 1).pair;
  std::printf("[ pair ] %s / %s\n", mesh.Stats(0, 1).pair.c_str(), mesh.Stats(1, 0).pair.c_str());
  EXPECT_EQ(mesh.Stats(0, 1).generation, 1);
  EXPECT_EQ(mesh.Stats(1, 0).generation, 1);
  // The offer came from the lower id, and carried no ICE server credentials.
  ASSERT_FALSE(mesh.Signals().empty());
  EXPECT_EQ(mesh.Signals().front().at("k").get<std::string>(), "dl");

  mesh.At(0).link->Send("from a");
  mesh.At(1).link->Send("from b");
  ASSERT_TRUE(mesh.WaitFor([&] {
    return Count(mesh.At(1).received, "from a") == 1 && Count(mesh.At(0).received, "from b") == 1;
  }));
  EXPECT_EQ(mesh.At(1).received.front().from, mesh.At(0).id);
  EXPECT_EQ(mesh.At(1).received.front().slot, 0);
  EXPECT_EQ(mesh.At(0).received.front().slot, 1);
  // Keepalives echo: a round trip of about nothing on one machine.
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.Stats(0, 1).rtt_ms >= 0; }, 3s));
  EXPECT_LE(mesh.Stats(0, 1).rtt_ms, 20);

  for (int i = 0; i < 3; ++i)
  {
    // A request from the answering side waits out the offering side's second between builds.
    std::this_thread::sleep_for(1100ms);
    const int before = mesh.Stats(0, 1).generation;
    mesh.At(i % 2).link->Test("rebuild");
    ASSERT_TRUE(mesh.WaitFor([&] {
      return mesh.Stats(0, 1).generation > before && mesh.Stats(1, 0).generation > before &&
             mesh.AllUp();
    })) << "rebuild " << i;
    const std::string payload = "after rebuild " + std::to_string(i);
    mesh.At(0).link->Send(payload);
    ASSERT_TRUE(mesh.WaitFor([&] { return Count(mesh.At(1).received, payload) == 1; }));
  }
  for (int i = 0; i < 2; ++i)
  {
    EXPECT_EQ(mesh.Stats(i, 1 - i).auth_drops, 0u);
    EXPECT_EQ(mesh.Stats(i, 1 - i).replays, 0u);
  }
}

// Three Orcas: a link per pair, each packet to both others.
TEST(OrcaDirectLink, ThreeOrcasLinkEveryPair)
{
  Mesh mesh(3);
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.AllUp(); }));
  mesh.At(1).link->Send("from b");
  ASSERT_TRUE(mesh.WaitFor([&] {
    return Count(mesh.At(0).received, "from b") == 1 && Count(mesh.At(2).received, "from b") == 1;
  }));
  EXPECT_EQ(Count(mesh.At(1).received, "from b"), 0);
  // The peer the link belongs to, never another.
  for (const auto& r : mesh.At(2).received)
    EXPECT_EQ(r.slot, r.from == mesh.At(0).id ? 0 : 1);
}

// Blocked datagrams (ORCA_DIRECT_BLOCK, `test-direct off`) leave the inputs on the relay until
// they flow again; relayed candidates only (ORCA_DIRECT_FORCE=turn) offer no host address at all,
// and with no TURN server here no link opens.
TEST(OrcaDirectLink, BlockedOrTurnOnlyStaysOnTheRelay)
{
  {
    Mesh mesh(2, [](DirectOptions& o, int i) { o.blocked = i == 0; });
    EXPECT_FALSE(mesh.WaitFor([&] { return mesh.Up(0, 1) || mesh.Up(1, 0); }, 1500ms));
    mesh.At(0).link->Test("on");
    ASSERT_TRUE(mesh.WaitFor([&] { return mesh.AllUp(); }));
    mesh.At(0).link->Test("in");
    ASSERT_TRUE(mesh.WaitFor([&] { return !mesh.Up(0, 1) && mesh.Up(1, 0); }, 3s));
    mesh.At(0).link->Test("on");
    ASSERT_TRUE(mesh.WaitFor([&] { return mesh.AllUp(); }));
  }
  {
    Mesh mesh(2, [](DirectOptions& o, int) { o.force = "turn"; });
    EXPECT_FALSE(mesh.WaitFor([&] { return mesh.Up(0, 1) || mesh.Up(1, 0); }, 1500ms));
    for (const picojson::object& signal : mesh.Signals())
    {
      if (signal.count("desc"))
        EXPECT_EQ(signal.at("desc").get<std::string>().find("a=candidate"), std::string::npos);
    }
  }
}

// The test delay (ORCA_TEST_NET_DELAY_MS) holds each datagram on the way out and again on the way
// in, as the room does for relayed messages: 30 ms on both machines is 60 ms each way, a round
// trip of 120 ms on the link.
TEST(OrcaDirectLink, TestDelayCoversTheLinkBothWays)
{
  Mesh mesh(2, [](DirectOptions& o, int) { o.net_delay_ms = 30; });
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.AllUp() && mesh.Stats(0, 1).rtt_ms >= 0; }));
  ASSERT_TRUE(mesh.WaitFor([&] { return mesh.Stats(0, 1).rtt_ms >= 110; }, 3s))
      << mesh.Stats(0, 1).rtt_ms;
  EXPECT_LE(mesh.Stats(0, 1).rtt_ms, 160);
}
