// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

// Orca's own game patches, forced on in every session of a disc profile (not a player option, and
// never the player's own GameSettings codes, which a session drops): Data/Sys/Orca/<game>.patches,
// hashed into the lobby compatibility key. One 32-bit word per line:
//
//   <address> <original> <value>   # comment
//   <address> * <value>             # comment
//
// all hex. A line with an original is a code patch for a module the game loads at that address:
// it is written only while the original word is there (the module is loaded and not yet patched),
// so it lands once per load and never into whatever else later occupies that memory. A `*` line is
// written every frame. Applied at each frame boundary through the session's frame hook (first runs
// and re-runs, before anything is saved), through Dolphin's memory-patch path, which invalidates
// the JIT's copy of changed code. A pure function of emulated memory: the same on both machines.
namespace Orca::UX
{
struct GamePatch
{
  u32 address = 0;
  std::optional<u32> original;
  u32 value = 0;
};

// The file's lines; nullopt and a reason for the first malformed one (the session then runs
// without them, and the key says so: the file's bytes are what is hashed).
std::optional<std::vector<GamePatch>> ParseGamePatches(std::string_view text, std::string* error);

// The active disc profile's patches (loaded once); empty for a launcher profile or none.
void ApplyGamePatches(const Core::CPUThreadGuard& guard);
}  // namespace Orca::UX
