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

  // Without a checksum, and with no pads.
  Packet bare;
  bare.current_frame = 5;
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

// Two players in one dev room on the real rooms Worker: both reach Playing on different seats with
// the same seed, and a packet crosses. ORCA_LIVE_TEST=1 (ORCA_SITE picks another site).
TEST(OrcaLive, TwoRoomsPlayEachOther)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  setenv("ORCA_TEST_DEV_GAME", "orca-live-test", 1);
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
  options.player_name = "Orca B";
  YouGameRoom b(options);
  for (int i = 0; i < 1500 && !(a.Connected() && b.Connected()); ++i)
  {
    if (a.GetState() == RoomState::Ended || b.GetState() == RoomState::Ended)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  unsetenv("ORCA_TEST_DEV_GAME");
  ASSERT_TRUE(a.Connected()) << a.Status();
  ASSERT_TRUE(b.Connected()) << b.Status();
  EXPECT_EQ(a.Seat(), 0);
  EXPECT_EQ(b.Seat(), 1);
  EXPECT_EQ(a.Seed(), b.Seed());
  EXPECT_EQ(a.Code(), code);

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
  // A's seat speaking as B's is refused, and ends the room for B.
  Packet forged = Sample();
  forged.seat = b.Seat();
  a.Send(forged);
  for (int i = 0; i < 500 && b.Connected(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(b.Connected());

  // Leaving reports the match void and closes; the other side sees its friend go.
  b.Leave();
  for (int i = 0; i < 500 && a.Connected(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(a.Connected()) << a.Status();
  EXPECT_GE(a.RoundTripMs(), 0);
}
