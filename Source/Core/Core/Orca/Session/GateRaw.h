// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

namespace Orca::Net
{
class Session;
}

namespace Orca::Gate
{
enum class Event
{
  Begin,
  StallStart,
  StallEnd,
  HitchTiming,
  LocalHitch,
  LoadHitch,
  Summary,
  Shutdown
};

// Observation only: no Session decision reads these fields. Only EmitRaw writes them.
struct RawState
{
  bool begun = false;
  bool pending = false;
  unsigned long long next_id = 1;
  int frame = 0;
  double began_ms = 0;
  int frames_ahead = 0;
  bool after_input = false;
  bool plugged = false;
  int settle_frame = 0;
  double hitch_ms = 0;
  double held_ms = 0;
};

void EmitRaw(const Net::Session& session, Event event,
             std::chrono::steady_clock::duration elapsed = {},
             std::chrono::steady_clock::duration held = {});
}  // namespace Orca::Gate

// Include only Session.cpp's macro branch: the evaluator requires the one textual
// definition there, and both translation units need that same definition.
#define ORCA_GATE_RAW_MACRO_ONLY
#include "Core/Orca/Session/Session.cpp"
#undef ORCA_GATE_RAW_MACRO_ONLY
