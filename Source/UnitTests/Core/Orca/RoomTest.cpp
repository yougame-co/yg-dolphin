// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "Core/Orca/Session/YouGameRoom.h"

using namespace Orca::Net;

namespace
{
void SetEnv(const char* name, const char* value)
{
#ifdef _WIN32
  _putenv_s(name, value ? value : "");
#else
  if (value)
    setenv(name, value, 1);
  else
    unsetenv(name);
#endif
}

Packet Sample()
{
  Packet packet;
  packet.seat = 1;
  packet.first_frame = 120;
  for (u8 i = 0; i < 3; ++i)
    packet.pads.push_back(Pad{i, 0xff, 0x80, 0x7f, 0, 1, static_cast<u8>(0xa0 + i), 0x0f});
  packet.ack = {130, -1, -1, -1};
  packet.current_frame = 121;
  packet.checksum_frame = 60;
  packet.checksum = 0xfedcba9876543210ull;
  packet.resimulated = 42;
  packet.advantage = -17;
  packet.rtt = 64;
  packet.stalls = 3;
  packet.hitches = 5;
  return packet;
}

void ExpectSame(const Packet& a, const Packet& b)
{
  EXPECT_EQ(a.seat, b.seat);
  EXPECT_EQ(a.first_frame, b.first_frame);
  EXPECT_EQ(a.pads, b.pads);
  EXPECT_EQ(a.ack, b.ack);
  EXPECT_EQ(a.current_frame, b.current_frame);
  EXPECT_EQ(a.checksum_frame, b.checksum_frame);
  EXPECT_EQ(a.checksum, b.checksum);
  EXPECT_EQ(a.resimulated, b.resimulated);
  EXPECT_EQ(a.advantage, b.advantage);
  EXPECT_EQ(a.rtt, b.rtt);
  EXPECT_EQ(a.stalls, b.stalls);
  EXPECT_EQ(a.hitches, b.hitches);
}

Packet Frames(int seat, int first, int count)
{
  Packet packet;
  packet.seat = seat;
  packet.first_frame = first;
  for (int f = first; f < first + count; ++f)
    packet.pads.push_back(Pad{static_cast<u8>(f), 0, 0, 0, 0, 0, 0, 0});
  packet.current_frame = first + count;
  return packet;
}
}  // namespace

TEST(OrcaRoom, PacketRoundTrips)
{
  const Packet sent = Sample();
  Packet got;
  ASSERT_TRUE(DecodePacket(EncodePacket(sent), &got));
  ExpectSame(sent, got);

  // Without a checksum, re-simulation count or advantage, and with no pads.
  Packet bare;
  bare.current_frame = 5;
  ASSERT_TRUE(DecodePacket(EncodePacket(bare), &got));
  ExpectSame(bare, got);
  EXPECT_EQ(got.resimulated, -1);
  EXPECT_FALSE(got.advantage);
  EXPECT_FALSE(got.rtt);
  EXPECT_EQ(got.stalls, -1);
  EXPECT_EQ(got.hitches, -1);
  const std::string text = EncodePacket(bare);
  EXPECT_EQ(text.find("\"r\""), std::string::npos);
  EXPECT_EQ(text.find("\"v\""), std::string::npos);
  EXPECT_EQ(text.find("\"l\""), std::string::npos);
  EXPECT_EQ(text.find("\"n\""), std::string::npos);
  EXPECT_EQ(text.find("\"j\""), std::string::npos);

  // A lead of zero, a quiet window and an instant relay still travel.
  bare.resimulated = 0;
  bare.advantage = 0;
  bare.rtt = 0;
  bare.stalls = 0;
  bare.hitches = 0;
  ASSERT_TRUE(DecodePacket(EncodePacket(bare), &got));
  ExpectSame(bare, got);
}

TEST(OrcaRoom, RejectsMalformedPackets)
{
  Packet got;
  const std::string good = EncodePacket(Sample());
  ASSERT_TRUE(DecodePacket(good, &got));
  const auto with = [&](const std::string& from, const std::string& to) {
    std::string text = good;
    const size_t at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    return at == std::string::npos ? text : text.replace(at, from.size(), to);
  };
  EXPECT_FALSE(DecodePacket("not json", &got));
  EXPECT_FALSE(DecodePacket("[]", &got));
  EXPECT_FALSE(DecodePacket(with("\"k\":\"p\"", "\"k\":\"q\""), &got));
  EXPECT_FALSE(DecodePacket(with("\"s\":1", "\"s\":4"), &got));
  EXPECT_FALSE(DecodePacket(with("\"s\":1", "\"s\":0.5"), &got));
  EXPECT_FALSE(DecodePacket(with("\"f\":120", "\"f\":-1"), &got));
  EXPECT_FALSE(DecodePacket(with("\"f\":120", "\"f\":1e300"), &got));
  EXPECT_FALSE(DecodePacket(with("\"c\":121", "\"c\":\"121\""), &got));
  // Pads: odd length, not hex, not whole pads.
  EXPECT_FALSE(DecodePacket(with("\"p\":\"00", "\"p\":\"0"), &got));
  EXPECT_FALSE(DecodePacket(with("\"p\":\"00", "\"p\":\"zz"), &got));
  EXPECT_FALSE(DecodePacket(with("\"p\":\"00", "\"p\":\"0000"), &got));
  EXPECT_FALSE(DecodePacket(with("[130,-1,-1,-1]", "[130,-1,-1]"), &got));
  EXPECT_FALSE(DecodePacket(with("[130,-1,-1,-1]", "[130,-2,-1,-1]"), &got));
  EXPECT_FALSE(DecodePacket(with("\"h\":\"fedcba9876543210\"", "\"h\":\"fedcba98\""), &got));
  // Telemetry (re-simulation count, lead, round trip, stalls, hitches) never refuses a packet,
  // which would end the match: out of range is clamped, a fraction truncated, anything not a number
  // ignored. (Exponents never reach the parser: SafeToParse refuses them for every field.)
  const auto telemetry = [&](const std::string& from, const std::string& to) {
    Packet out;
    EXPECT_TRUE(DecodePacket(with(from, to), &out)) << to;
    return out;
  };
  EXPECT_EQ(telemetry("\"r\":42", "\"r\":-1").resimulated, 0);
  EXPECT_EQ(telemetry("\"r\":42", "\"r\":4.5").resimulated, 4);
  EXPECT_EQ(telemetry("\"r\":42", "\"r\":999999999999").resimulated, 1 << 20);
  EXPECT_EQ(telemetry("\"r\":42", "\"r\":\"42\"").resimulated, -1);
  EXPECT_EQ(telemetry("\"v\":-17", "\"v\":-1.5").advantage, -1);
  EXPECT_EQ(telemetry("\"v\":-17", "\"v\":-999999999999").advantage, -(1 << 20));
  EXPECT_EQ(telemetry("\"v\":-17", "\"v\":null").advantage, std::nullopt);
  EXPECT_EQ(telemetry("\"v\":-17", "\"v\":1000").advantage, 1000);
  EXPECT_EQ(telemetry("\"l\":64", "\"l\":-5").rtt, 0);
  EXPECT_EQ(telemetry("\"l\":64", "\"l\":999999999999").rtt, 1 << 20);
  EXPECT_EQ(telemetry("\"l\":64", "\"l\":[]").rtt, std::nullopt);
  EXPECT_EQ(telemetry("\"n\":3", "\"n\":-1").stalls, 0);
  EXPECT_EQ(telemetry("\"n\":3", "\"n\":2.5").stalls, 2);
  EXPECT_EQ(telemetry("\"n\":3", "\"n\":999999999999").stalls, 1 << 20);
  EXPECT_EQ(telemetry("\"n\":3", "\"n\":\"3\"").stalls, -1);
  EXPECT_EQ(telemetry("\"j\":5", "\"j\":-2").hitches, 0);
  EXPECT_EQ(telemetry("\"j\":5", "\"j\":999999999999").hitches, 1 << 20);
  EXPECT_EQ(telemetry("\"j\":5", "\"j\":true").hitches, -1);

  // What would crash the JSON parser: deep nesting, and numbers that overflow a double.
  EXPECT_FALSE(DecodePacket(std::string(5000, '[') + std::string(5000, ']'), &got));
  EXPECT_FALSE(DecodePacket(with("\"c\":121", "\"c\":1e999"), &got));
  EXPECT_FALSE(DecodePacket(with("\"c\":121", "\"c\":" + std::string(400, '9')), &got));
  // Words with an e inside strings, and true/false, are fine.
  EXPECT_TRUE(DecodePacket(with("\"k\":\"p\"", "\"e\":\"eEe\",\"t\":true,\"u\":false,\"k\":\"p\""), &got));

  // More pads than a packet may carry.
  Packet big = Frames(0, 0, MAX_PADS_IN_PACKET + 1);
  EXPECT_FALSE(DecodePacket(EncodePacket(big), &got));
  big.pads.pop_back();
  EXPECT_TRUE(DecodePacket(EncodePacket(big), &got));
}

TEST(OrcaRoom, CoalescingKeepsEarlierInputsAndChecksum)
{
  std::optional<Packet> pending;
  Packet first = Frames(0, 10, 3);  // frames 10-12
  first.checksum_frame = 0;
  first.checksum = 42;
  CoalescePacket(&pending, first);
  // The next one starts later (frames 12-14; the peer acknowledged up to 11 meanwhile, as far as
  // the sender knows) and carries no checksum.
  CoalescePacket(&pending, Frames(0, 12, 3));
  ASSERT_TRUE(pending);
  EXPECT_EQ(pending->first_frame, 10);
  ASSERT_EQ(pending->pads.size(), 5u);
  for (int i = 0; i < 5; ++i)
    EXPECT_EQ(pending->pads[i][0], 10 + i);
  EXPECT_EQ(pending->checksum_frame, 0);
  EXPECT_EQ(pending->checksum, 42u);

  // A packet that starts at or before the pending one simply replaces it.
  CoalescePacket(&pending, Frames(0, 8, 9));
  EXPECT_EQ(pending->first_frame, 8);
  EXPECT_EQ(pending->pads.size(), 9u);
  EXPECT_EQ(pending->checksum_frame, 0);

  // Never more than a packet may carry: the newest inputs win.
  pending = Frames(0, 0, 40);
  CoalescePacket(&pending, Frames(0, 30, 40));  // frames 30-69
  EXPECT_EQ(pending->pads.size(), static_cast<size_t>(MAX_PADS_IN_PACKET));
  EXPECT_EQ(pending->first_frame, 70 - MAX_PADS_IN_PACKET);
  EXPECT_EQ(pending->pads.front()[0], 70 - MAX_PADS_IN_PACKET);
}

TEST(OrcaRoom, RefusesToStartOutsideTheApp)
{
  // No bridge and no dev game: the room ends at once with a reason, without touching the network.
  if (std::getenv("YOUGAME_BRIDGE") || std::getenv("ORCA_TEST_DEV_GAME"))
    GTEST_SKIP() << "a bridge or dev game is configured";
  YouGameRoom room({});
  for (int i = 0; i < 100 && room.GetState() != RoomState::Ended; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(room.GetState(), RoomState::Ended);
  EXPECT_FALSE(room.Connected());
  EXPECT_FALSE(room.Status().empty());
}

// Waits until `room` reports another player arrived; returns its event.
std::optional<PeerEvent> WaitArrived(YouGameRoom& room)
{
  for (int i = 0; i < 1500 && room.GetState() != RoomState::Ended; ++i)
  {
    for (const PeerEvent& event : room.TakePeerEvents())
    {
      if (event.kind == PeerEvent::Kind::Arrived)
        return event;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return std::nullopt;
}

// A host and a joiner in one dev room on the real rooms Worker: each sees the other arrive on its
// seat, a packet and a keyframe offer cross, a forged packet is dropped, and the host sees the
// joiner leave while its own room stays. ORCA_LIVE_TEST=1 (ORCA_SITE picks another site).
TEST(OrcaLive, TwoRoomsPlayEachOther)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  SetEnv("ORCA_TEST_DEV_GAME", "orca-live-test");
  const std::string code =
      "orca" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000);
  RoomOptions options;
  options.room_code = code;
  options.compatibility = "orca-live-test";
  options.player_name = "Orca A";
  YouGameRoom a(options);
  // The first in is the host; give it a head start so the seats are predictable.
  for (int i = 0; i < 500 && a.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(a.GetState(), RoomState::Waiting) << a.Status();
  EXPECT_EQ(a.Joining(), false);
  options.player_name = "Orca B";
  options.joining = true;
  YouGameRoom b(options);
  const auto b_seen = WaitArrived(a);
  const auto a_seen = WaitArrived(b);
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
  ASSERT_TRUE(b_seen) << a.Status();
  ASSERT_TRUE(a_seen) << b.Status();
  EXPECT_EQ(b.Joining(), true);
  EXPECT_EQ(a.Seat(), 0);
  EXPECT_EQ(b.Seat(), 1);
  EXPECT_EQ(b_seen->seat, 1);
  EXPECT_FALSE(b_seen->host);
  EXPECT_EQ(a_seen->seat, 0);
  EXPECT_TRUE(a_seen->host);
  EXPECT_EQ(a.Code(), code);

  KeyframeInfo offer{120, "kf-test", 1234, "00112233aabbccdd"};
  a.OfferKeyframe(1, offer);
  std::optional<PeerEvent> offered;
  for (int i = 0; i < 500 && !offered; ++i)
  {
    for (const PeerEvent& event : b.TakePeerEvents())
    {
      if (event.kind == PeerEvent::Kind::Keyframe)
        offered = event;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(offered);
  EXPECT_EQ(offered->keyframe.id, offer.id);
  EXPECT_EQ(offered->keyframe.frame, offer.frame);
  EXPECT_EQ(offered->keyframe.size, offer.size);
  EXPECT_EQ(offered->keyframe.hash, offer.hash);

  Packet sent = Sample();
  sent.seat = a.Seat();
  a.Send(sent);
  std::vector<Packet> got;
  for (int i = 0; i < 500 && got.empty(); ++i)
  {
    got = b.Receive();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(got.size(), 1u);
  ExpectSame(sent, got[0]);
  // A's seat speaking as B's is dropped.
  Packet forged = Sample();
  forged.seat = b.Seat();
  a.Send(forged);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_TRUE(b.Receive().empty());
  EXPECT_TRUE(b.Connected());

  // The joiner leaves; the host's room stays, and says who left.
  b.Leave();
  std::optional<PeerEvent> left;
  for (int i = 0; i < 500 && !left; ++i)
  {
    for (const PeerEvent& event : a.TakePeerEvents())
    {
      if (event.kind == PeerEvent::Kind::Left)
        left = event;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(left);
  EXPECT_EQ(left->seat, 1);
  EXPECT_TRUE(a.Connected()) << a.Status();
  EXPECT_GE(a.RoundTripMs(), 0);
}

// The test network delay: one side holds every message 100 ms each way, the other sets the variable
// to 0 (both are then test processes, so their keys match). The delayed side's round trip shows it.
TEST(OrcaLive, TestNetworkDelayHoldsMessages)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  SetEnv("ORCA_SESSION", "1");
  SetEnv("ORCA_TEST_DEV_GAME", "orca-live-test");
  const std::string code =
      "orcad" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 10000000);
  RoomOptions options;
  options.room_code = code;
  options.compatibility = "orca-live-test";
  SetEnv("ORCA_TEST_NET_DELAY_MS", "100");
  YouGameRoom a(options);
  for (int i = 0; i < 500 && a.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  SetEnv("ORCA_TEST_NET_DELAY_MS", "0");
  options.joining = true;
  YouGameRoom b(options);
  WaitArrived(a);
  WaitArrived(b);
  SetEnv("ORCA_TEST_NET_DELAY_MS", nullptr);
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
  SetEnv("ORCA_SESSION", nullptr);
  ASSERT_TRUE(a.Connected()) << a.Status();
  ASSERT_TRUE(b.Connected()) << b.Status();

  // A packet from b reaches a's inbox no sooner than a's inbound delay.
  const auto sent_at = std::chrono::steady_clock::now();
  Packet sent = Sample();
  sent.seat = b.Seat();
  b.Send(sent);
  std::vector<Packet> got;
  for (int i = 0; i < 300 && got.empty(); ++i)
  {
    got = a.Receive();
    if (got.empty())
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_EQ(got.size(), 1u);
  EXPECT_GE(std::chrono::steady_clock::now() - sent_at, std::chrono::milliseconds(100));
  // Pings go out held and pongs come back held: the round trip includes both delays.
  for (int i = 0; i < 300 && a.RoundTripMs() < 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_GE(a.RoundTripMs(), 200);
  EXPECT_LT(b.RoundTripMs(), 200);
}

// Under the desktop app: one room through the app's bridge (YOUGAME_BRIDGE, YOUGAME_TOKEN,
// YOUGAME_GAME, set by the test's caller to Tools/orca/fake_bridge.py), the other on a dev ticket. The bridged
// side mirrors the room to the page, and the page's Leave ends it. ORCA_LIVE_TEST=1 and
// ORCA_TEST_BRIDGE_ROOM=<the fake bridge's room code>.
TEST(OrcaLive, BridgeMirrorsTheRoomAndTakesLeave)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  const char* room = std::getenv("ORCA_TEST_BRIDGE_ROOM");
  if (!live || std::string(live) != "1" || !room || !std::getenv("YOUGAME_BRIDGE"))
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1, ORCA_TEST_BRIDGE_ROOM and a bridge";
  RoomOptions options;
  options.compatibility = "orca-live-test";
  options.mode = "orca-test";
  // The app's invite names the room (hello.room).
  YouGameRoom a(options);
  for (int i = 0; i < 1000 && a.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(a.GetState(), RoomState::Waiting) << a.Status();
  EXPECT_EQ(a.Code(), room);

  // The friend joins without the app.
  const std::string bridge = std::getenv("YOUGAME_BRIDGE");
  SetEnv("YOUGAME_BRIDGE", nullptr);
  SetEnv("ORCA_TEST_DEV_GAME", "orca-bridge-test");
  options.room_code = room;
  YouGameRoom b(options);
  // The room reads its environment on its own thread: restore it once b has its ticket.
  for (int i = 0; i < 1000 && b.Code().empty() && b.GetState() != RoomState::Ended; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
  SetEnv("YOUGAME_BRIDGE", bridge.c_str());
  for (int i = 0; i < 1500 && !(a.Connected() && b.Connected()); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_TRUE(a.Connected()) << a.Status();
  ASSERT_TRUE(b.Connected()) << b.Status();

  // The fake bridge sends the page's Leave two seconds after the room reads "ready".
  for (int i = 0; i < 1000 && a.GetState() != RoomState::Ended; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(a.GetState(), RoomState::Ended);
  EXPECT_EQ(a.Status(), "You left the room");
  EXPECT_EQ(a.ErrorCode(), "");
  for (int i = 0; i < 500 && b.Connected(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(b.Connected()) << b.Status();
}

TEST(OrcaRoom, RefusalsSayWhy)
{
  // WebSocket::Connect's error carries the upgrade's HTTP status.
  EXPECT_NE(RefusalReason("WebSocket connect failed: ... (HTTP 403)").find("differs"),
            std::string::npos);
  EXPECT_EQ(RefusalReason("WebSocket connect failed: ... (HTTP 409)"), "That room is full");
  EXPECT_NE(RefusalReason("WebSocket connect failed: ... (HTTP 401)").find("expired"),
            std::string::npos);
  EXPECT_EQ(RefusalReason("WebSocket connect failed: Couldn't connect to server (HTTP 0)"),
            "Could not reach YouGame's rooms");
}

// Two Orcas whose compatibility keys differ never share a room: the second is told why.
TEST(OrcaLive, DifferentKeysAreRefusedWithAReason)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  SetEnv("ORCA_TEST_DEV_GAME", "orca-live-test");
  RoomOptions options;
  options.room_code =
      "orcak" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 10000000);
  options.compatibility = "orca-key-a";
  YouGameRoom a(options);
  for (int i = 0; i < 500 && a.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(a.GetState(), RoomState::Waiting) << a.Status();
  options.compatibility = "orca-key-b";
  YouGameRoom b(options);
  for (int i = 0; i < 1000 && b.GetState() != RoomState::Ended; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
  EXPECT_EQ(b.GetState(), RoomState::Ended);
  EXPECT_NE(b.Status().find("differs"), std::string::npos) << b.Status();
  EXPECT_EQ(b.ErrorCode(), "room_mismatch");
  EXPECT_EQ(a.GetState(), RoomState::Waiting);
  EXPECT_EQ(a.ErrorCode(), "");
  a.Leave();
  EXPECT_EQ(a.ErrorCode(), "");
}

TEST(OrcaRoom, RefusalsHaveAppCodes)
{
  EXPECT_EQ(RefusalCode("WebSocket connect failed: ... (HTTP 403)"), "room_mismatch");
  EXPECT_EQ(RefusalCode("WebSocket connect failed: ... (HTTP 409)"), "room_full");
  EXPECT_EQ(RefusalCode("WebSocket connect failed: ... (HTTP 0)"), "network");
}

// A desync is reported as one on both sides, never as a friend who left. Under drop-in play:
// a joiner that leaves on a desync says so in its bye, and the host's room reports the joiner's
// departure with that reason; a host that unplugs a joiner for a desync tells it, and the joiner's
// room ends with the desync code; a host that leaves on a desync says so to whoever is left.
TEST(OrcaLive, DesyncIsReportedOnBothSides)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  SetEnv("ORCA_TEST_DEV_GAME", "orca-live-test");
  RoomOptions options;
  options.room_code =
      "orcab" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 10000000);
  options.compatibility = "orca-live-test";
  YouGameRoom host(options);
  for (int i = 0; i < 500 && host.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(host.GetState(), RoomState::Waiting) << host.Status();
  options.joining = true;
  const auto left_reason = [&host]() -> std::optional<std::string> {
    for (int i = 0; i < 500; ++i)
    {
      for (const PeerEvent& event : host.TakePeerEvents())
      {
        if (event.kind == PeerEvent::Kind::Left)
          return event.reason;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::nullopt;
  };

  // 1. The joiner leaves on a desync.
  {
    YouGameRoom joiner(options);
    ASSERT_TRUE(WaitArrived(host));
    ASSERT_TRUE(WaitArrived(joiner));
    joiner.SetLeaveReason("desync");
    joiner.Leave();
    EXPECT_EQ(left_reason(), std::optional<std::string>("desync"));
    EXPECT_EQ(joiner.ErrorCode(), "");
  }
  // A plain leave carries no reason.
  {
    YouGameRoom joiner(options);
    ASSERT_TRUE(WaitArrived(host));
    ASSERT_TRUE(WaitArrived(joiner));
    joiner.Leave();
    EXPECT_EQ(left_reason(), std::optional<std::string>(""));
  }
  // 2. The host unplugs the joiner for a desync.
  {
    YouGameRoom joiner(options);
    ASSERT_TRUE(WaitArrived(host));
    ASSERT_TRUE(WaitArrived(joiner));
    host.DropPeer(joiner.Seat(), "desync");
    for (int i = 0; i < 500 && joiner.GetState() != RoomState::Ended; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(joiner.GetState(), RoomState::Ended);
    EXPECT_EQ(joiner.ErrorCode(), "desync") << joiner.Status();
    joiner.Leave();
    left_reason();
  }
  // 3. The host leaves on a desync: the joiner hears the reason with the host's departure.
  {
    YouGameRoom joiner(options);
    ASSERT_TRUE(WaitArrived(host));
    ASSERT_TRUE(WaitArrived(joiner));
    host.SetLeaveReason("desync");
    host.Leave();
    std::optional<PeerEvent> gone;
    for (int i = 0; i < 500 && !gone; ++i)
    {
      for (const PeerEvent& event : joiner.TakePeerEvents())
      {
        if (event.kind == PeerEvent::Kind::Left)
          gone = event;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(gone);
    EXPECT_TRUE(gone->host);
    EXPECT_EQ(gone->reason, "desync");
    EXPECT_EQ(host.ErrorCode(), "");
  }
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
}

// A room waiting for a friend stays open for as long as the friend takes: half a minute of pings
// and no other traffic. (Windows lost its socket after about 8 s of waiting, 2026-10-01.)
TEST(OrcaLive, WaitingRoomStaysOpen)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  SetEnv("ORCA_TEST_DEV_GAME", "orca-live-test");
  RoomOptions options;
  options.room_code =
      "orcaw" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 10000000);
  options.compatibility = "orca-live-test";
  YouGameRoom a(options);
  for (int i = 0; i < 500 && a.GetState() == RoomState::Connecting; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  SetEnv("ORCA_TEST_DEV_GAME", nullptr);
  ASSERT_EQ(a.GetState(), RoomState::Waiting) << a.Status();
  for (int second = 0; second < 30; ++second)
  {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ASSERT_EQ(a.GetState(), RoomState::Waiting) << "after " << second + 1 << " s: " << a.Status();
  }
  EXPECT_GE(a.RoundTripMs(), 0);
}
