// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

// The YouGame desktop app's controller snapshots (schema 1: desktop/src/controllers.ts
// ControllerSnapshot, docs/controllers/implementation-contract.md) and how Orca turns one into the
// local player's GameCube pad. Pure functions: no I/O, no clocks, so the unit tests cover all of it.
// The pad only decides which bytes this player sends; both machines then decode the same bytes
// (Session/PadCodec.h), so nothing here is in the compatibility key.
namespace Orca::UX
{
// One physical port of a GameCube adapter, raw as the helper read it from the 0x21 report.
struct AdapterPort
{
  int port = 0;  // 0-3, the adapter's physical port
  int seat = 0;  // 0-3, the player the Controls panel gave it (seat 0 is this player)
  bool connected = false;
  bool wireless = false;
  u16 buttons = 0;  // wire mask: byte 1 | (byte 2 & 0xF) << 8 (A B X Y Left Right Down Up, Start Z R L)
  std::array<u8, 4> axes{128, 128, 128, 128};  // stick X, Y, C-stick X, Y
  std::array<u8, 2> triggers{0, 0};            // L, R
  std::array<u8, 6> origin{128, 128, 128, 128, 0, 0};  // the Controls panel's calibration neutral
};

// An SDL gamepad with the W3C "standard" layout (17 buttons, 4 axes).
struct StandardPad
{
  int index = 0;
  std::array<float, 4> axes{};      // -1..1: left X, left Y (down +), right X, right Y (down +)
  std::array<float, 17> buttons{};  // 0..1
  std::array<bool, 17> pressed{};
};

struct Snapshot
{
  u64 sequence = 0;
  double sent_at = 0;      // helper clock, epoch ms
  double received_at = 0;  // helper clock: the last valid adapter report (0: none yet)
  bool owned = false;      // the helper holds an adapter (then ports has all four)
  bool suspended = false;
  std::vector<AdapterPort> ports;
  std::vector<StandardPad> pads;  // standard-mapped pads only, lowest index first
};

// One SSE `data:` payload, `{"type":"controllers","snapshot":{...}}`. Anything malformed, out of
// range or of another schema is refused whole (nullopt): a bad snapshot never reaches a pad.
std::optional<Snapshot> ParseSnapshotEvent(std::string_view json);

// Input older than this is neutral (the contract's 250 ms; the helper republishes every 100 ms).
constexpr double STALE_MS = 250;

// Connected, nothing pressed, sticks centred: what a stale or suspended source gives.
GCPadStatus NeutralPad();

// An adapter port's bytes as Dolphin's own GCAdapter decodes them (GCAdapter::DecodeChannel), then
// the calibration origin as an offset: raw - origin + centre for sticks, raw - origin for
// triggers, clamped. The default origin is the identity, so an uncalibrated port is byte-for-byte
// Dolphin's adapter path (Orca's SI origin is always centred, so the game applies none itself).
GCPadStatus MapAdapterPort(const AdapterPort& port);

// A standard gamepad on a GameCube layout: A = south, B = west, X = east, Y = north, Z = either
// bumper, L/R = the triggers (analog, digital past 90%), Start, d-pad, both sticks.
GCPadStatus MapStandardPad(const StandardPad& pad);

enum class PadSource
{
  Adapter,
  Gamepad,
};

struct LocalChoice
{
  PadSource source;
  int index;  // adapter port, or the pad's index
  GCPadStatus pad;
  bool neutral;  // stale or suspended
};

// Which pad is this player's: the adapter port on the lowest seat with a controller in it (seat 0,
// the one the player claimed, when it is plugged in), else the first standard gamepad; nullopt
// when there is neither (the caller falls back to Dolphin's mapping, GCPadNew.ini).
// `age_ms` is how long ago this machine received the snapshot (its own steady clock): staleness is
// never judged by comparing two processes' wall clocks.
std::optional<LocalChoice> ChooseLocalPad(const Snapshot& snapshot, double age_ms);
}  // namespace Orca::UX
