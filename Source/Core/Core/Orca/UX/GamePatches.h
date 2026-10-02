// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

// Orca's own game patches, forced on in every session (not a player option, and never the player's
// own GameSettings codes, which a session drops): Data/Sys/Orca/<profile>.patches, hashed into the
// lobby compatibility key. A launcher profile's file (PPLUS32.patches) applies only once its frame
// hook is live, after the loader has handed over to the game, and the launcher's own codes own most
// of that memory: such a file holds only guarded code groups of the disc's own modules. One 32-bit
// word per line:
//
//   <address> <original> <value>   # comment
//   <address> * <value>             # comment
//
// all hex. A line with an original is a code patch for a module the game loads at that address.
// Consecutive such lines (each 4 bytes after the last) are one group, written only while every word
// of the group is its original: the module is loaded and not yet patched, so the group lands once
// per load, whole, and never into whatever else later occupies that memory (a single common word
// would match some later module by chance; a group's run of words does not). A line whose original
// equals its value only guards. A `*` line is data, written every frame. Applied at each frame
// boundary through the session's frame hook (first runs and re-runs, before anything is saved);
// code words through Dolphin's memory-patch path, which invalidates the JIT's copy. A pure function
// of emulated memory: the same on both machines.
namespace Orca::UX
{
struct GamePatch
{
  u32 address = 0;
  std::optional<u32> original;
  u32 value = 0;
  // Part of the previous line's group (both have originals, this one 4 bytes after it).
  bool joins_previous = false;
};

// The file's lines; nullopt and a reason for the first malformed one or a repeated address.
std::optional<std::vector<GamePatch>> ParseGamePatches(std::string_view text, std::string* error);

// Whether one group (or one `*` line) is written, given what memory holds at its addresses now.
bool GroupApplies(std::span<const GamePatch> group, std::span<const u32> now);

// The active profile's patches (loaded when the profile changes); nothing without a profile or a
// file. A malformed file is a session error (Orca::Status "profile") and is not applied.
void ApplyGamePatches(const Core::CPUThreadGuard& guard);
}  // namespace Orca::UX
