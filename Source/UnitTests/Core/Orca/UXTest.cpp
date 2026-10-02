// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <map>
#include <set>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "Core/Orca/UX/ControllerSource.h"
#include "Core/Orca/UX/Controllers.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/Overlay.h"
#include "InputCommon/GCPadStatus.h"

using namespace Orca::UX;

namespace
{
// Dolphin's adapter decoding written out from the wire layout (GCAdapter.cpp ProcessInputPayload,
// before it was split into DecodeChannel): an independent reference for the parity test.
GCPadStatus ReferenceDecode(const std::array<u8, 9>& c)
{
  GCPadStatus pad = {};
  constexpr u16 b1_bits[8] = {PAD_BUTTON_A,    PAD_BUTTON_B,     PAD_BUTTON_X,    PAD_BUTTON_Y,
                              PAD_BUTTON_LEFT, PAD_BUTTON_RIGHT, PAD_BUTTON_DOWN, PAD_BUTTON_UP};
  constexpr u16 b2_bits[4] = {PAD_BUTTON_START, PAD_TRIGGER_Z, PAD_TRIGGER_R, PAD_TRIGGER_L};
  for (int i = 0; i < 8; ++i)
    if (c[1] & (1 << i))
      pad.button |= b1_bits[i];
  for (int i = 0; i < 4; ++i)
    if (c[2] & (1 << i))
      pad.button |= b2_bits[i];
  pad.stickX = c[3];
  pad.stickY = c[4];
  pad.substickX = c[5];
  pad.substickY = c[6];
  pad.triggerLeft = c[7];
  pad.triggerRight = c[8];
  return pad;
}

void ExpectSame(const GCPadStatus& a, const GCPadStatus& b)
{
  EXPECT_EQ(a.button, b.button);
  EXPECT_EQ(a.stickX, b.stickX);
  EXPECT_EQ(a.stickY, b.stickY);
  EXPECT_EQ(a.substickX, b.substickX);
  EXPECT_EQ(a.substickY, b.substickY);
  EXPECT_EQ(a.triggerLeft, b.triggerLeft);
  EXPECT_EQ(a.triggerRight, b.triggerRight);
  EXPECT_EQ(a.analogA, b.analogA);
  EXPECT_EQ(a.analogB, b.analogB);
  EXPECT_EQ(a.isConnected, b.isConnected);
}

std::string PortJson(int port, int seat, const char* type, int buttons,
                     std::array<int, 4> axes = {128, 128, 128, 128},
                     std::array<int, 2> triggers = {0, 0},
                     std::array<int, 6> origin = {128, 128, 128, 128, 0, 0})
{
  const bool connected = std::string(type) != "null";
  return fmt::format(
      R"({{"adapterId":"s-gc","port":{},"seat":{},"connected":{},"type":{},"buttons":{},)"
      R"("axes":[{},{},{},{}],"triggers":[{},{}],"origin":[{},{},{},{},{},{}],"calibrationRevision":0}})",
      port, seat, connected ? "true" : "false", connected ? fmt::format("\"{}\"", type) : "null",
      buttons, axes[0], axes[1], axes[2], axes[3], triggers[0], triggers[1], origin[0], origin[1],
      origin[2], origin[3], origin[4], origin[5]);
}

std::string PadJson(int index, const char* mapping, const std::string& axes,
                    const std::string& buttons)
{
  return fmt::format(
      R"({{"index":{},"id":"Xbox Wireless Controller 045e:0b13","connected":true,"mapping":"{}","axes":{},"buttons":{},"timestamp":1759370000123.25}})",
      index, mapping, axes, buttons);
}

std::string Buttons17(int pressed_index = -1, double value = 1)
{
  std::string s = "[";
  for (int i = 0; i < 17; ++i)
  {
    if (i)
      s += ',';
    s += i == pressed_index ? fmt::format(R"({{"value":{},"pressed":true}})", value) :
                              R"({"value":0,"pressed":false})";
  }
  return s + "]";
}

std::string Event(bool owned, const std::string& ports, const std::string& pads,
                  double sent = 1759370000500.125, double received = 1759370000499.5,
                  bool suspended = false, int schema = 1)
{
  return fmt::format(
      R"({{"type":"controllers","snapshot":{{"schema":{},"session":"0b6f","sequence":7,"sentAt":{:.3f},)"
      R"("receivedAt":{:.3f},"source":"native","state":"connected","message":"","owned":{},)"
      R"("suspended":{},"invalidReports":0,"ports":[{}],"pads":[{}]}}}})",
      schema, sent, received, owned ? "true" : "false", suspended ? "true" : "false", ports, pads);
}

std::string FourPorts(const std::string& p0 = PortJson(0, 0, "null", 0),
                      const std::string& p1 = PortJson(1, 1, "null", 0),
                      const std::string& p2 = PortJson(2, 2, "null", 0),
                      const std::string& p3 = PortJson(3, 3, "null", 0))
{
  return p0 + "," + p1 + "," + p2 + "," + p3;
}
}  // namespace

TEST(OrcaUXControllers, AdapterBytesMatchDolphinsAdapterForEveryButtonMask)
{
  for (const bool wireless : {false, true})
  {
    for (int buttons = 0; buttons < 4096; ++buttons)
    {
      AdapterPort port;
      port.connected = true;
      port.wireless = wireless;
      port.buttons = static_cast<u16>(buttons);
      port.axes = {static_cast<u8>(buttons & 0xFF), static_cast<u8>(255 - (buttons & 0xFF)),
                   static_cast<u8>((buttons * 7) & 0xFF), static_cast<u8>((buttons * 13) & 0xFF)};
      port.triggers = {static_cast<u8>((buttons * 3) & 0xFF), static_cast<u8>((buttons >> 4) & 0xFF)};
      const std::array<u8, 9> wire{wireless ? u8(0x20) : u8(0x10),
                                   static_cast<u8>(buttons & 0xFF),
                                   static_cast<u8>(buttons >> 8),
                                   port.axes[0],
                                   port.axes[1],
                                   port.axes[2],
                                   port.axes[3],
                                   port.triggers[0],
                                   port.triggers[1]};
      ExpectSame(MapAdapterPort(port), ReferenceDecode(wire));
      if (::testing::Test::HasFailure())
        return;
    }
  }
}

TEST(OrcaUXControllers, EveryAxisByteIsPassedThroughUncalibrated)
{
  for (int v = 0; v < 256; ++v)
  {
    AdapterPort port;
    port.connected = true;
    port.axes = {u8(v), u8(v), u8(v), u8(v)};
    port.triggers = {u8(v), u8(v)};
    const GCPadStatus pad = MapAdapterPort(port);
    EXPECT_EQ(pad.stickX, v);
    EXPECT_EQ(pad.stickY, v);
    EXPECT_EQ(pad.substickX, v);
    EXPECT_EQ(pad.substickY, v);
    EXPECT_EQ(pad.triggerLeft, v);
    EXPECT_EQ(pad.triggerRight, v);
    // PAD_GET_ORIGIN and PAD_USE_ORIGIN are calibration state the session never sends.
    EXPECT_EQ(pad.button & (PAD_GET_ORIGIN | PAD_USE_ORIGIN), 0);
  }
}

TEST(OrcaUXControllers, CalibrationOriginIsAnOffsetAndClamps)
{
  AdapterPort port;
  port.connected = true;
  port.origin = {131, 124, 126, 130, 30, 20};
  port.axes = {131, 124, 126, 130};
  port.triggers = {30, 20};
  GCPadStatus pad = MapAdapterPort(port);
  EXPECT_EQ(pad.stickX, 128);
  EXPECT_EQ(pad.stickY, 128);
  EXPECT_EQ(pad.substickX, 128);
  EXPECT_EQ(pad.substickY, 128);
  EXPECT_EQ(pad.triggerLeft, 0);
  EXPECT_EQ(pad.triggerRight, 0);

  port.axes = {255, 0, 200, 60};
  port.triggers = {10, 255};
  pad = MapAdapterPort(port);
  EXPECT_EQ(pad.stickX, 252);    // 255 - 131 + 128
  EXPECT_EQ(pad.stickY, 4);      // 0 - 124 + 128
  EXPECT_EQ(pad.substickX, 202);
  EXPECT_EQ(pad.substickY, 58);
  EXPECT_EQ(pad.triggerLeft, 0);  // below its origin: clamped
  EXPECT_EQ(pad.triggerRight, 235);

  port.origin = {100, 160, 128, 128, 0, 0};
  port.axes = {250, 10, 128, 128};
  pad = MapAdapterPort(port);
  EXPECT_EQ(pad.stickX, 255);  // 278 clamps
  EXPECT_EQ(pad.stickY, 0);    // -22 clamps
}

TEST(OrcaUXControllers, StandardPadOnAGameCubeLayout)
{
  StandardPad pad;
  EXPECT_EQ(MapStandardPad(pad).button, 0);
  EXPECT_EQ(MapStandardPad(pad).stickX, 128);
  EXPECT_EQ(MapStandardPad(pad).stickY, 128);

  const auto only = [](int index) {
    StandardPad p;
    p.buttons[index] = 1;
    p.pressed[index] = true;
    return MapStandardPad(p).button;
  };
  EXPECT_EQ(only(0), PAD_BUTTON_A);
  EXPECT_EQ(only(2), PAD_BUTTON_B);
  EXPECT_EQ(only(1), PAD_BUTTON_X);
  EXPECT_EQ(only(3), PAD_BUTTON_Y);
  EXPECT_EQ(only(4), PAD_TRIGGER_Z);
  EXPECT_EQ(only(5), PAD_TRIGGER_Z);
  EXPECT_EQ(only(9), PAD_BUTTON_START);
  EXPECT_EQ(only(12), PAD_BUTTON_UP);
  EXPECT_EQ(only(13), PAD_BUTTON_DOWN);
  EXPECT_EQ(only(14), PAD_BUTTON_LEFT);
  EXPECT_EQ(only(15), PAD_BUTTON_RIGHT);
  EXPECT_EQ(only(8), 0);   // Back
  EXPECT_EQ(only(16), 0);  // Home

  // Analog triggers: travel, and the click at the end.
  StandardPad p;
  p.buttons[6] = 0.5f;
  p.buttons[7] = 0.95f;
  p.pressed[7] = true;
  GCPadStatus s = MapStandardPad(p);
  EXPECT_EQ(s.triggerLeft, 128);
  EXPECT_EQ(s.triggerRight, 242);
  EXPECT_EQ(s.button, PAD_TRIGGER_R);

  // Sticks: right and down on the pad are right and down on the GC (its Y grows upward).
  p = {};
  p.axes = {1, 1, -1, -1};
  s = MapStandardPad(p);
  EXPECT_EQ(s.stickX, 255);
  EXPECT_EQ(s.stickY, 1);
  EXPECT_EQ(s.substickX, 1);
  EXPECT_EQ(s.substickY, 255);
  p.axes = {0.5f, -0.25f, 0, 0};
  s = MapStandardPad(p);
  EXPECT_EQ(s.stickX, 192);  // 128 + 63.5
  EXPECT_EQ(s.stickY, 160);  // 128 + 31.75
}

TEST(OrcaUXControllers, ParsesTheHelpersSnapshot)
{
  const std::string json =
      Event(true,
            FourPorts(PortJson(0, 0, "wired", 0x101, {140, 120, 128, 128}, {33, 0}),
                      PortJson(1, 1, "wireless", 0x800), PortJson(2, 2, "null", 0),
                      PortJson(3, 3, "null", 0)),
            PadJson(3, "standard", "[0.00003051850947599719,-1,0.5,-0.000030518509475997192]",
                    Buttons17(0)) +
                "," + PadJson(1, "", "[0,0,0,0,0,0]", "[]") + "," +
                PadJson(2, "standard", "[0,0,0,0]", Buttons17()));
  const auto s = ParseSnapshotEvent(json);
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s->sequence, 7u);
  EXPECT_DOUBLE_EQ(s->sent_at, 1759370000500.125);
  EXPECT_TRUE(s->owned);
  ASSERT_EQ(s->ports.size(), 4u);
  EXPECT_TRUE(s->ports[0].connected);
  EXPECT_FALSE(s->ports[0].wireless);
  EXPECT_EQ(s->ports[0].buttons, 0x101);
  EXPECT_EQ(s->ports[0].axes[0], 140);
  EXPECT_EQ(s->ports[0].triggers[0], 33);
  EXPECT_TRUE(s->ports[1].wireless);
  EXPECT_FALSE(s->ports[2].connected);
  // The unmapped joystick is skipped; the standard ones are sorted by index.
  ASSERT_EQ(s->pads.size(), 2u);
  EXPECT_EQ(s->pads[0].index, 2);
  EXPECT_EQ(s->pads[1].index, 3);
  EXPECT_FLOAT_EQ(s->pads[1].axes[1], -1);
  EXPECT_TRUE(s->pads[1].pressed[0]);

  // Not owned: no ports at all.
  const auto none = ParseSnapshotEvent(Event(false, "", ""));
  ASSERT_TRUE(none.has_value());
  EXPECT_TRUE(none->ports.empty());
  EXPECT_TRUE(none->pads.empty());
}

TEST(OrcaUXControllers, RefusesMalformedSnapshots)
{
  const std::string good = FourPorts();
  EXPECT_TRUE(ParseSnapshotEvent(Event(true, good, "")).has_value());
  EXPECT_FALSE(ParseSnapshotEvent(Event(true, good, "", 1, 1, false, 2)).has_value());  // schema
  EXPECT_FALSE(ParseSnapshotEvent(Event(false, good, "")).has_value());  // ports without owning
  EXPECT_FALSE(ParseSnapshotEvent(Event(true, "", "")).has_value());     // owned without ports
  EXPECT_FALSE(ParseSnapshotEvent(
                   Event(true, FourPorts(PortJson(1, 0, "wired", 0)), ""))  // port out of order
                   .has_value());
  EXPECT_FALSE(
      ParseSnapshotEvent(Event(true, FourPorts(PortJson(0, 0, "wired", 4096)), "")).has_value());
  EXPECT_FALSE(
      ParseSnapshotEvent(Event(true, FourPorts(PortJson(0, 0, "usb", 0)), "")).has_value());
  EXPECT_FALSE(ParseSnapshotEvent(Event(true, FourPorts(PortJson(0, 0, "wired", 0, {256, 0, 0, 0})),
                                        ""))
                   .has_value());
  EXPECT_FALSE(ParseSnapshotEvent(
                   Event(true, FourPorts(PortJson(0, 0, "wired", 0, {1, 2, 3, 4}, {0, 0},
                                                  {128, 128, 128, 128, 0, -1})),
                         ""))
                   .has_value());
  EXPECT_FALSE(ParseSnapshotEvent(Event(false, "", PadJson(0, "standard", "[0,0,0]", Buttons17())))
                   .has_value());
  EXPECT_FALSE(
      ParseSnapshotEvent(Event(false, "", PadJson(0, "standard", "[2,0,0,0]", Buttons17())))
          .has_value());
  EXPECT_FALSE(ParseSnapshotEvent(R"({"type":"other","snapshot":{}})").has_value());
  EXPECT_FALSE(ParseSnapshotEvent("not json").has_value());
  EXPECT_FALSE(ParseSnapshotEvent(std::string(100, '[') + std::string(100, ']')).has_value());
  EXPECT_FALSE(ParseSnapshotEvent(R"({"type":"controllers","n":1e999})").has_value());
  EXPECT_FALSE(
      ParseSnapshotEvent(R"({"type":"controllers","n":)" + std::string(400, '9') + "}").has_value());
}

TEST(OrcaUXControllers, ChoosesTheClaimedAdapterPortThenTheFirstGamepad)
{
  const std::string pad = PadJson(0, "standard", "[0,0,0,0]", Buttons17(0));
  // Seat 0 (the port the player claimed, here physical port 2) wins over a lower port.
  auto s = ParseSnapshotEvent(Event(true,
                                    FourPorts(PortJson(0, 2, "wired", 0x02), PortJson(1, 1, "null", 0),
                                              PortJson(2, 0, "wired", 0x01), PortJson(3, 3, "null", 0)),
                                    pad));
  ASSERT_TRUE(s);
  auto c = ChooseLocalPad(*s, 0);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->source, PadSource::Adapter);
  EXPECT_EQ(c->index, 2);
  EXPECT_EQ(c->pad.button, PAD_BUTTON_A);
  EXPECT_FALSE(c->neutral);

  // Seat 0 empty: the lowest seat with a controller.
  s = ParseSnapshotEvent(Event(true,
                               FourPorts(PortJson(0, 0, "null", 0), PortJson(1, 3, "wired", 0x04),
                                         PortJson(2, 1, "wired", 0x08), PortJson(3, 2, "null", 0)),
                               pad));
  c = ChooseLocalPad(*s, 0);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->index, 2);
  EXPECT_EQ(c->pad.button, PAD_BUTTON_Y);

  // An adapter with nothing plugged in: the gamepad.
  s = ParseSnapshotEvent(Event(true, FourPorts(), pad));
  c = ChooseLocalPad(*s, 0);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->source, PadSource::Gamepad);
  EXPECT_EQ(c->pad.button, PAD_BUTTON_A);

  // Nothing at all: Dolphin's own mapping.
  s = ParseSnapshotEvent(Event(false, "", ""));
  EXPECT_FALSE(ChooseLocalPad(*s, 0).has_value());
}

TEST(OrcaUXControllers, StaleOrSuspendedInputIsNeutral)
{
  const std::string pad = PadJson(0, "standard", "[1,0,0,0]", Buttons17(0));
  const std::string ports = FourPorts(PortJson(0, 0, "wired", 0x01, {200, 128, 128, 128}));
  // Fresh: the report is 0.625 ms older than the snapshot.
  auto s = ParseSnapshotEvent(Event(true, ports, pad));
  EXPECT_FALSE(ChooseLocalPad(*s, 100)->neutral);
  EXPECT_EQ(ChooseLocalPad(*s, 100)->pad.stickX, 200);
  // The snapshot itself is old here.
  auto c = ChooseLocalPad(*s, 251);
  EXPECT_TRUE(c->neutral);
  EXPECT_EQ(c->source, PadSource::Adapter);
  EXPECT_EQ(c->pad.button, 0);
  EXPECT_EQ(c->pad.stickX, 128);
  EXPECT_TRUE(c->pad.isConnected);
  // The adapter's last report is old even though the snapshot is new (USB stalled).
  s = ParseSnapshotEvent(Event(true, ports, pad, 1759370000500.0, 1759370000300.0));
  EXPECT_FALSE(ChooseLocalPad(*s, 40)->neutral);
  EXPECT_TRUE(ChooseLocalPad(*s, 60)->neutral);
  // No report yet.
  s = ParseSnapshotEvent(Event(true, ports, pad, 1759370000500.0, 0));
  EXPECT_TRUE(ChooseLocalPad(*s, 0)->neutral);
  // Suspended.
  s = ParseSnapshotEvent(Event(true, ports, pad, 1759370000500.0, 1759370000500.0, true));
  EXPECT_TRUE(ChooseLocalPad(*s, 0)->neutral);
  // The helper failed (the app's fail(): suspended, every port emptied, no pads): Dolphin's own
  // mapping takes over instead of leaving the player stuck neutral.
  s = ParseSnapshotEvent(Event(true, FourPorts(), "", 1759370000500.0, 1759370000400.0, true));
  ASSERT_TRUE(s);
  EXPECT_FALSE(ChooseLocalPad(*s, 0).has_value());
  // A gamepad goes stale with its snapshot.
  s = ParseSnapshotEvent(Event(false, "", pad));
  EXPECT_FALSE(ChooseLocalPad(*s, 250)->neutral);
  EXPECT_TRUE(ChooseLocalPad(*s, 250.5)->neutral);
  EXPECT_EQ(ChooseLocalPad(*s, 250.5)->pad.stickX, 128);
}


namespace
{
std::vector<Orca::Events::PortInfo> Ports(std::initializer_list<Orca::Events::PortInfo> list)
{
  return list;
}
}  // namespace

TEST(OrcaUXOverlay, ToastsFollowThePortListOnceEach)
{
  OverlayModel m;
  // The solo host's own port at boot: no toast.
  m.OnPorts(0, Ports({{0, "luis", false}}), 0);
  EXPECT_TRUE(m.View(10, false).toasts.empty());
  // A friend plugs in; a re-run of that frame gives the same list again: one toast.
  m.OnPorts(300, Ports({{0, "luis", false}, {1, "ada", true}}), 1000);
  m.OnPorts(300, Ports({{0, "luis", false}, {1, "ada", true}}), 1010);
  auto v = m.View(1100, false);
  ASSERT_EQ(v.toasts.size(), 1u);
  EXPECT_EQ(v.toasts[0].text, "ada joined on port 2");
  EXPECT_FLOAT_EQ(v.toasts[0].alpha, 1);
  // It fades over its last half second and is gone after three.
  EXPECT_NEAR(m.View(3750, false).toasts[0].alpha, 0.5f, 1e-4);
  EXPECT_TRUE(m.View(4001, false).toasts.empty());
  // The friend unplugs.
  m.OnPorts(900, Ports({{0, "luis", false}}), 5000);
  v = m.View(5001, false);
  ASSERT_EQ(v.toasts.size(), 1u);
  EXPECT_EQ(v.toasts[0].text, "ada left");
  // Joining a friend's game takes the frame number back to theirs: still shown.
  m.OnPorts(120, Ports({{0, "bo", true}, {1, "luis", false}}), 5100);
  const auto joined = m.View(5101, false).toasts;
  ASSERT_EQ(joined.size(), 2u);
  EXPECT_EQ(joined[1].text, "bo joined on port 1");

  // A joiner's first list already holds the host.
  OverlayModel joiner;
  joiner.OnPorts(0, Ports({{0, "luis", true}, {1, "ada", false}}), 0);
  ASSERT_EQ(joiner.View(1, false).toasts.size(), 1u);
  EXPECT_EQ(joiner.View(1, false).toasts[0].text, "luis is on port 1");
}

TEST(OrcaUXOverlay, WaitingOnlyAfterAQuarterSecondOfStall)
{
  OverlayModel m;
  m.OnPorts(0, Ports({{0, "luis", false}, {1, "ada", true}}), 0);
  LinkView link;
  link.online = true;
  link.round_trip_ms = 40;
  link.stalled_ms = 200;
  link.waiting_seat = 1;
  m.Update(link, 100);
  EXPECT_EQ(m.View(100, false).waiting, "");
  link.stalled_ms = 260;
  m.Update(link, 160);
  EXPECT_EQ(m.View(160, false).waiting, "Waiting for ada\u2026");
  EXPECT_EQ(m.View(160, false).signal, Signal::Bad);
  // The session that doesn't report stall time yet (-1) never shows it.
  link.stalled_ms = -1;
  m.Update(link, 200);
  EXPECT_EQ(m.View(200, false).waiting, "");
  // Offline: nothing at all.
  link.online = false;
  link.stalled_ms = 900;
  m.Update(link, 300);
  EXPECT_FALSE(m.View(300, false).show_signal);
  EXPECT_EQ(m.View(300, false).waiting, "");
}

TEST(OrcaUXOverlay, SignalFromRoundTripAndRecentStalls)
{
  LinkView link;
  link.online = true;
  link.stalled_ms = 0;
  const auto signal_at = [](LinkView l, std::vector<std::pair<double, int>> stalls) {
    OverlayModel m;
    for (auto [t, n] : stalls)
    {
      l.stalls = n;
      m.Update(l, t);
    }
    return m.View(stalls.back().first, false).signal;
  };
  link.round_trip_ms = 30;
  EXPECT_EQ(signal_at(link, {{0, 5}, {1000, 5}}), Signal::Good);
  EXPECT_EQ(signal_at(link, {{0, 5}, {1000, 7}}), Signal::Ok);
  EXPECT_EQ(signal_at(link, {{0, 5}, {1000, 15}}), Signal::Bad);
  // Stalls older than three seconds no longer count.
  EXPECT_EQ(signal_at(link, {{0, 5}, {1000, 15}, {4500, 15}}), Signal::Good);
  link.round_trip_ms = 90;
  EXPECT_EQ(signal_at(link, {{0, 0}}), Signal::Ok);
  link.round_trip_ms = 160;
  EXPECT_EQ(signal_at(link, {{0, 0}}), Signal::Bad);
  link.round_trip_ms = -1;  // no measurement yet
  EXPECT_EQ(signal_at(link, {{0, 0}}), Signal::Good);
}

TEST(OrcaUXOverlay, StatsReadoutOnlyWhenAskedAndOnline)
{
  OverlayModel m;
  LinkView link;
  link.online = true;
  link.round_trip_ms = 42;
  link.delay = 2;
  link.rollbacks_per_second = 1.25;
  m.Update(link, 0);
  EXPECT_EQ(m.View(0, false).stats, "");
  EXPECT_EQ(m.View(0, true).stats, "42 ms \u00b7 delay 2 \u00b7 1.2 rb/s");
  EXPECT_TRUE(m.View(0, true).show_signal);
  link.online = false;
  m.Update(link, 1);
  EXPECT_EQ(m.View(1, true).stats, "");
}


namespace
{
// Brawl's memory as the name-tag writer walks it, laid out like rev 2 on the character select.
class FakeBrawl final : public GuestMemory
{
public:
  static constexpr u32 SCENE = 0x90ff6340, NAME = 0x806ff278, TASK = 0x815e75e0,
                       GLOBAL = 0x90181300, RECORDS = 0x90172d40;
  static u32 Area(int i) { return 0x81500000 + static_cast<u32>(i) * 0x1000; }
  static u32 Tag(int i) { return RECORDS + 0xE0 + static_cast<u32>(i) * 0x124; }

  explicit FakeBrawl(const char* scene = "scSelctCharacter")
  {
    W32(0x805A0060, 0x805b8ba0);
    W32(0x805b8ba0 + 4, SCENE);
    W32(SCENE, NAME);
    for (size_t i = 0; i <= std::strlen(scene); ++i)
      bytes[NAME + static_cast<u32>(i)] = static_cast<u8>(scene[i]);
    W32(SCENE + 0x400, TASK);
    W32(0x805A00E0, GLOBAL);
    for (u32 i = 0; i < CONTROLS.size(); ++i)
      bytes[0x80406938 + i] = CONTROLS[i];
    W32(GLOBAL + 0x28, RECORDS);
    for (u32 a = RECORDS; a < Tag(120); a += 2)
      Write16(a, 0);
    for (int i = 0; i < 4; ++i)
    {
      W32(TASK + 0x44 + 4 * static_cast<u32>(i), Area(i));
      for (u32 a = Area(i); a < Area(i) + 0x448; a += 4)
        W32(a, 0);
      W32(Area(i) + 0x1B0, static_cast<u32>(i));
      W32(Area(i) + 0x1B4, 0);
      W32(Area(i) + 0x1C8, 0xFFFFFFFF);
    }
  }
  void Join(int i) { W32(Area(i) + 0x1B4, 1); }
  // Brawl rev 2's default controls (RAM 0x80406938, read from a dump): GameCube, Wii Remote,
  // with Nunchuk, Classic.
  static constexpr std::array<u8, 0x2D> CONTROLS{
      0x03, 0x03, 0x04, 0x0a, 0x0b, 0x0c, 0x00, 0x01, 0x05, 0x02, 0x02, 0x80, 0x01, 0x00, 0x05,
      0x09, 0x03, 0x04, 0x02, 0x04, 0x0a, 0x0c, 0x0b, 0x00, 0x01, 0x04, 0x03, 0x02, 0x02, 0x04,
      0x03, 0x40, 0x03, 0x03, 0x04, 0x04, 0x0a, 0x0b, 0x0c, 0x00, 0x01, 0x05, 0x02, 0x02, 0x80};
  void SetTag(int i, const std::u16string& name)
  {
    for (u32 k = 0; k < 6; ++k)
      Write16(Tag(i) + 2 * k, k < name.size() ? static_cast<u16>(name[k]) : 0);
  }
  std::u16string TagName(int i) const
  {
    std::u16string n;
    for (u32 k = 0; k < 5 && Read16(Tag(i) + 2 * k); ++k)
      n.push_back(static_cast<char16_t>(Read16(Tag(i) + 2 * k)));
    return n;
  }
  s32 NameId(int i) const { return static_cast<s32>(Read32(Area(i) + 0x1C8)); }

  bool Valid(u32 a) const override { return bytes.contains(a); }
  u8 Read8(u32 a) const override { return bytes.at(a); }
  u16 Read16(u32 a) const override { return static_cast<u16>(Read8(a) << 8 | Read8(a + 1)); }
  u32 Read32(u32 a) const override { return u32(Read16(a)) << 16 | Read16(a + 2); }
  void Write8(u32 a, u8 v) override
  {
    bytes[a] = v;
    ++writes;
    if (recording)
      written.insert(a);
  }
  void Write16(u32 a, u16 v) override
  {
    bytes[a] = static_cast<u8>(v >> 8);
    bytes[a + 1] = static_cast<u8>(v);
    ++writes;
    if (recording)
    {
      written.insert(a);
      written.insert(a + 1);
    }
  }
  void Write32(u32 a, u32 v) override
  {
    Write16(a, static_cast<u16>(v >> 16));
    Write16(a + 2, static_cast<u16>(v));
  }
  void W32(u32 a, u32 v) { Write32(a, v); }

  std::map<u32, u8> bytes;
  int writes = 0;
  bool recording = false;
  std::set<u32> written;
};
}  // namespace

TEST(OrcaUXNameTags, UsernamesBecomeBrawlTags)
{
  EXPECT_EQ(BrawlTag("ada"), u"ADA");
  EXPECT_EQ(BrawlTag("crunchytester"), u"CRUNC");
  EXPECT_EQ(BrawlTag("sandbox-ada"), u"SANDB");
  EXPECT_EQ(BrawlTag("a_b c"), u"A-B-C");
  EXPECT_EQ(BrawlTag("x.y!?"), u"X.Y!?");
  EXPECT_EQ(BrawlTag("L0u1s"), u"L0U1S");
  EXPECT_EQ(BrawlTag("j\xc3\xa9r\xc3\xb4me"), u"JRME");  // non-ASCII dropped, never transliterated
  EXPECT_EQ(BrawlTag("@#$"), u"");
  EXPECT_EQ(BrawlTag(""), u"");
}

TEST(OrcaUXNameTags, JoinedPortsGetTheirTagOnTheCharacterSelect)
{
  FakeBrawl m;
  const std::vector<Orca::Events::PortInfo> ports{{0, "luis", false}, {1, "ada", true}};
  // Nobody has joined yet: nothing.
  EXPECT_EQ(ApplyNameTags(m, ports), 0);
  m.Join(0);
  m.Join(1);
  EXPECT_EQ(ApplyNameTags(m, ports), 2);
  // The highest unused tags, in port order.
  EXPECT_EQ(m.NameId(0), 119);
  EXPECT_EQ(m.TagName(119), u"LUIS");
  EXPECT_EQ(m.NameId(1), 118);
  EXPECT_EQ(m.TagName(118), u"ADA");
  EXPECT_EQ(m.NameId(2), -1);
  // Idempotent: the next frame (or a re-run of this one) writes nothing.
  const int writes = m.writes;
  EXPECT_EQ(ApplyNameTags(m, ports), 0);
  EXPECT_EQ(m.writes, writes);
}

TEST(OrcaUXNameTags, ReusesTheSameNameAndLeavesAChosenTagAlone)
{
  FakeBrawl m;
  m.SetTag(0, u"MINE");
  m.SetTag(3, u"ADA");
  m.Join(0);
  m.Join(1);
  // Port 0's player picked their own tag; port 1's name is already a tag.
  m.W32(FakeBrawl::Area(0) + 0x1C8, 0);
  EXPECT_EQ(ApplyNameTags(m, {{0, "luis", false}, {1, "ada", true}}), 1);
  EXPECT_EQ(m.NameId(0), 0);
  EXPECT_EQ(m.TagName(0), u"MINE");
  EXPECT_EQ(m.NameId(1), 3);
  EXPECT_EQ(m.TagName(119), u"");  // nothing new was made
}

TEST(OrcaUXNameTags, ANewTagIsMadeAsTheGameMakesOneAndNothingElseIsWritten)
{
  FakeBrawl m;
  // A leftover record (a deleted tag's bytes) in the slot that will be used.
  for (u32 a = FakeBrawl::Tag(119) + 0x60; a < FakeBrawl::Tag(119) + 0x80; a += 2)
    m.Write16(a, 0xAAAA);
  m.Join(0);
  m.recording = true;
  ASSERT_EQ(ApplyNameTags(m, {{0, "luis", false}}), 1);
  const u32 tag = FakeBrawl::Tag(119);
  EXPECT_EQ(m.TagName(119), u"LUIS");
  EXPECT_EQ(m.Read8(tag + 0x0C), 1);  // rumble on
  for (u32 i = 0; i < FakeBrawl::CONTROLS.size(); ++i)
    EXPECT_EQ(m.Read8(tag + 0x14 + i), FakeBrawl::CONTROLS[i]) << i;
  for (u32 a = tag + 0x60; a < tag + 0x80; ++a)
    EXPECT_EQ(m.Read8(a), 0);  // cleared
  // Only that record and port 0's tag field.
  for (const u32 a : m.written)
  {
    const bool in_tag = a >= tag && a < tag + 0x124;
    const bool in_name_id = a >= FakeBrawl::Area(0) + 0x1C8 && a < FakeBrawl::Area(0) + 0x1CC;
    EXPECT_TRUE(in_tag || in_name_id) << std::hex << a;
  }
}

TEST(OrcaUXNameTags, TwoPortsNeverShareATag)
{
  FakeBrawl m;
  m.Join(0);
  m.Join(1);
  m.Join(2);
  // Every sandbox account starts SANDB; a short name gets the digit appended.
  EXPECT_EQ(ApplyNameTags(m, {{0, "sandbox-ada", false},
                              {1, "sandbox-bo", true},
                              {2, "sandbox-cy", true}}),
            3);
  EXPECT_EQ(m.TagName(m.NameId(0)), u"SANDB");
  EXPECT_EQ(m.TagName(m.NameId(1)), u"SAND2");
  EXPECT_EQ(m.TagName(m.NameId(2)), u"SAND3");
  // A port joining later whose tag someone already wears: its variant.
  FakeBrawl late;
  late.Join(1);
  EXPECT_EQ(ApplyNameTags(late, {{0, "ada", false}, {1, "ada", true}}), 1);
  EXPECT_EQ(late.TagName(late.NameId(1)), u"ADA");
  late.Join(0);
  EXPECT_EQ(ApplyNameTags(late, {{0, "ada", false}, {1, "ada", true}}), 1);
  EXPECT_EQ(late.TagName(late.NameId(0)), u"ADA1");
  // One port joined, one not: only the joined one.
  FakeBrawl half;
  half.Join(1);
  EXPECT_EQ(ApplyNameTags(half, {{0, "luis", false}, {1, "ada", true}}), 1);
  EXPECT_EQ(half.NameId(0), -1);
  EXPECT_EQ(half.TagName(half.NameId(1)), u"ADA");
}

TEST(OrcaUXNameTags, UnmappedPointersWriteNothing)
{
  {
    FakeBrawl m;
    m.Join(0);
    m.W32(FakeBrawl::TASK + 0x44, 0x83000000);  // area pointer to nowhere
    m.recording = true;
    EXPECT_EQ(ApplyNameTags(m, {{0, "luis", false}}), 0);
    EXPECT_TRUE(m.written.empty());
  }
  {
    FakeBrawl m;
    m.Join(0);
    m.W32(FakeBrawl::SCENE + 0x400, 0x83000000);  // no task
    EXPECT_EQ(ApplyNameTags(m, {{0, "luis", false}}), 0);
  }
  {
    // The records' end not mapped.
    FakeBrawl m;
    m.Join(0);
    m.bytes.erase(FakeBrawl::Tag(120) - 1);
    EXPECT_EQ(ApplyNameTags(m, {{0, "luis", false}}), 0);
  }
}

TEST(OrcaUXNameTags, NothingOutsideTheCharacterSelectOrWhenItDoesNotLookRight)
{
  {
    FakeBrawl m("scMelee");
    m.Join(0);
    EXPECT_EQ(ApplyNameTags(m, {{0, "luis", false}}), 0);
  }
  {
    // An area that is not port 0's (another layout, a Project+ build that moved it): left alone.
    FakeBrawl m;
    m.Join(0);
    m.W32(FakeBrawl::Area(0) + 0x1B0, 2);
    EXPECT_EQ(ApplyNameTags(m, {{0, "luis", false}}), 0);
    EXPECT_EQ(m.NameId(0), -1);
  }
  {
    // A name with nothing Brawl can show, and a port out of range.
    FakeBrawl m;
    m.Join(0);
    EXPECT_EQ(ApplyNameTags(m, {{0, "\xe2\x98\x83", false}, {5, "ada", true}}), 0);
  }
  {
    // Every tag taken: no tag rather than someone else's.
    FakeBrawl m;
    for (int i = 0; i < 120; ++i)
      m.SetTag(i, u"T");
    m.Join(0);
    EXPECT_EQ(ApplyNameTags(m, {{0, "luis", false}}), 0);
  }
}


#ifndef _WIN32
namespace
{
// A one-connection SSE server on loopback: the desktop app's /controllers/events, scripted.
class FakeBridge
{
public:
  explicit FakeBridge(int status = 200) : m_status(status)
  {
    m_listen = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(m_listen, 4);
    socklen_t len = sizeof(addr);
    getsockname(m_listen, reinterpret_cast<sockaddr*>(&addr), &len);
    m_port = ntohs(addr.sin_port);
    m_thread = std::thread([this] { Serve(); });
  }
  ~FakeBridge()
  {
    Close();
    if (m_thread.joinable())
      m_thread.join();
  }
  std::string Url() const { return fmt::format("http://127.0.0.1:{}", m_port); }
  bool WaitConnected()
  {
    for (int i = 0; i < 200 && m_client < 0; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return m_client >= 0;
  }
  void Send(const std::string& json)
  {
    const std::string event = "data: " + json + "\n\n";
    if (m_client >= 0)
      (void)!write(m_client, event.data(), event.size());
  }
  void Close()
  {
    if (m_listen >= 0)
    {
      shutdown(m_listen, SHUT_RDWR);
      close(m_listen);
      m_listen = -1;
    }
    if (const int fd = m_client.exchange(-1); fd >= 0)
    {
      shutdown(fd, SHUT_RDWR);
      close(fd);
    }
  }
  std::string Request()
  {
    std::lock_guard lk(m_request_lock);
    return request;
  }
  std::string request;
  std::atomic<int> connections{0};

private:
  void Serve()
  {
    // Every connection until Close(): a client that retries is counted, not left in the backlog.
    for (;;)
    {
      const int fd = accept(m_listen, nullptr, nullptr);
      if (fd < 0)
        return;
      ++connections;
      char buf[4096];
      const ssize_t n = read(fd, buf, sizeof(buf));
      {
        std::lock_guard lk(m_request_lock);
        request.assign(buf, n > 0 ? size_t(n) : 0);
      }
      const std::string head =
          m_status == 200 ?
              "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\ncache-control: no-store\r\n\r\n" :
              fmt::format(
                  "HTTP/1.1 {} No\r\ncontent-type: application/json\r\ncontent-length: 2\r\n\r\n{{}}",
                  m_status);
      (void)!write(fd, head.data(), head.size());
      if (m_status != 200)
      {
        close(fd);
        continue;
      }
      if (m_client >= 0)
        close(fd);  // one stream at a time, like the app
      else
        m_client = fd;
    }
  }
  std::mutex m_request_lock;
  int m_status;
  int m_listen = -1;
  std::atomic<int> m_client{-1};
  int m_port = 0;
  std::thread m_thread;
};

template <typename F>
bool Eventually(F f, int ms = 2000)
{
  for (int waited = 0; waited < ms; waited += 5)
  {
    if (f())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return f();
}
}  // namespace

TEST(OrcaUXControllerStream, CachesTheNewestSnapshotAndGoesNeutralThenFallsBack)
{
  FakeBridge bridge;
  StartControllerStream(bridge.Url(), "t0ken");
  ASSERT_TRUE(bridge.WaitConnected());
  EXPECT_NE(bridge.Request().find("GET /controllers/events "), std::string::npos);
  EXPECT_NE(bridge.Request().find("Authorization: Bearer t0ken"), std::string::npos);
  EXPECT_FALSE(LatestPad().has_value());

  const auto now_ms = [] {
    return std::chrono::duration<double, std::milli>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  const auto send = [&](int buttons, int stick_x) {
    const double t = now_ms();
    bridge.Send(Event(true, FourPorts(PortJson(0, 0, "wired", buttons, {stick_x, 128, 128, 128})),
                      "", t, t));
  };
  const auto pad_is = [](u16 buttons, u8 stick_x) {
    return [buttons, stick_x] {
      const auto p = LatestPad();
      return p && p->button == buttons && p->stickX == stick_x;
    };
  };
  send(0x01, 170);
  ASSERT_TRUE(Eventually(pad_is(PAD_BUTTON_A, 170)));
  // Several reports between two frames: only the newest counts.
  send(0x02, 100);
  send(0x04, 90);
  ASSERT_TRUE(Eventually(pad_is(PAD_BUTTON_X, 90)));
  // A malformed event is refused and changes nothing.
  bridge.Send(R"({"type":"controllers","snapshot":{"schema":9}})");
  ASSERT_TRUE(Eventually([] { return GetControllerTiming().refused >= 1; }));
  send(0x04, 90);
  ASSERT_TRUE(Eventually(pad_is(PAD_BUTTON_X, 90)));
  EXPECT_TRUE(GetControllerTiming().attached);
  EXPECT_GE(GetControllerTiming().samples, 2u);
  // A restarted helper counts its sequence from 1 again: still taken.
  {
    const double t = now_ms();
    std::string restarted = Event(true, FourPorts(PortJson(0, 0, "wired", 0x08, {60, 128, 128, 128})),
                                  "", t, t);
    const std::string from = R"("session":"0b6f","sequence":7)";
    restarted.replace(restarted.find(from), from.size(), R"("session":"other","sequence":1)");
    bridge.Send(restarted);
  }
  ASSERT_TRUE(Eventually(pad_is(PAD_BUTTON_Y, 60)));

  // Nothing new for 250 ms: neutral, not the last held button.
  ASSERT_TRUE(Eventually(pad_is(0, 128), 600));

  // The app closes the stream: still neutral a moment, then none (Dolphin's own mapping).
  bridge.Close();
  ASSERT_TRUE(Eventually([] { return !GetControllerTiming().attached; }));
  ASSERT_TRUE(Eventually([] { return !LatestPad().has_value(); }, 2500));
  StopControllerStream();
}

TEST(OrcaUXControllerStream, RefusedTokenStopsAndNonLoopbackNeverConnects)
{
  {
    FakeBridge bridge(401);
    StartControllerStream(bridge.Url(), "bad");
    ASSERT_TRUE(Eventually([&] { return bridge.connections.load() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EXPECT_EQ(bridge.connections.load(), 1);  // no retry (a retry would be counted)
    EXPECT_FALSE(LatestPad().has_value());
    StopControllerStream();
  }
  // Only http://127.0.0.1:<port>: the token goes nowhere else.
  StartControllerStream("http://example.com:80", "t0ken");
  EXPECT_FALSE(GetControllerTiming().attached);
  EXPECT_FALSE(LatestPad().has_value());
  StopControllerStream();
}

// The transport's added latency against the real desktop app (bridge.ts + controllers.ts) with a
// scripted helper: Tools/orca/controller-latency.mjs sets these and runs this test alone.
TEST(OrcaUXLatency, BridgeTransport)
{
  const char* bridge = std::getenv("ORCA_UX_LATENCY_BRIDGE");
  const char* token = std::getenv("ORCA_UX_LATENCY_TOKEN");
  if (!bridge || !token)
    GTEST_SKIP() << "set ORCA_UX_LATENCY_BRIDGE/TOKEN (Tools/orca/controller-latency.mjs)";
  const char* seconds_env = std::getenv("ORCA_UX_LATENCY_SECONDS");
  const int seconds = seconds_env ? std::atoi(seconds_env) : 10;
  StartControllerStream(bridge, token);
  ASSERT_TRUE(Eventually([] { return GetControllerTiming().samples > 10; }, 5000));
  ResetControllerTiming();
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  const ControllerTiming t = GetControllerTiming();
  fmt::print("orca ux latency snapshots={} samples={} refused={} p50={:.3f} p95={:.3f} p99={:.3f} "
             "max={:.3f} ms\n",
             t.snapshots, t.samples, t.refused, t.p50_ms, t.p95_ms, t.p99_ms, t.max_ms);
  EXPECT_EQ(t.refused, 0u);
  EXPECT_GT(t.samples, 0u);
  StopControllerStream();
}
#endif
