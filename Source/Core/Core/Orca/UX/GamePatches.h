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
// of that memory: such a file holds only guarded groups (code groups of the disc's own modules, and
// a data group that sets a default once). One 32-bit word per line:
//
//   <address> <original> <value>   # comment
//   <address> * <value>             # comment
//
// all hex. A line with an original is guarded. Consecutive such lines (each 4 bytes after the last)
// are one group, written only while every word of the group is its original. Most groups are code
// patches for a module the game loads at that address: the module is loaded and not yet patched, so
// the group lands once per load, whole, and never into whatever else later occupies that memory (a
// single common word would match some later module by chance; a group's run of words does not). A
// guarded data group (the rules' item default in gmGlobalRecord, 0x9017BE50) lands once, while the
// game's own defaults are there, and one of its words becomes a marker ('ORCA') so it never lands
// again: a player's later choice stays. A line whose original equals its value only guards. A `*`
// line is data, written every frame. Applied at each frame boundary through the session's frame
// hook (first runs and re-runs, before anything is saved); guarded words through Dolphin's
// memory-patch path, which invalidates the JIT's copy. A pure function of emulated memory: the same
// on both machines.
//
// Toggled groups follow a condition on emulated memory instead of landing once. Guarded lines
// between `when` lines and an `end` are toggled:
//
//   when <address> == <value>             # the word at <address> is <value>
//   when <address> & <mask> != <value>    # (word & mask) is not <value>
//   <address> <original> <value>          # a guarded line, grouped as above
//   end
//
// The block's `when` lines must all hold. While they do, a group whose words are all their
// originals gets its values; while they don't, a group whose words are all its values gets its
// originals back; anything else (the module not loaded, something else in its memory) is left
// alone, as a guarded group is. Every word either way goes through the memory-patch path, so the
// JIT drops its copy at each toggle (a rollback's restore drops it for the pages it copies back).
// The decision reads only emulated memory (the condition words and the group's own), so it is a pure
// function of memory: both machines, every re-run and a drop-in's keyframe (which carries whichever
// state memory had) decide the same at the same boundary. Toggled groups are applied last at each
// boundary, after every other writer of the frame hook, so a condition one of them sets is followed
// at that same boundary, and the hook run again there (a re-run from the snapshot saved there)
// writes nothing new. A group never spans a `when` or an `end`; blocks don't nest; a block holds
// no `*` line and at least one line; each toggled group has two words at least, one of them
// changing (it writes on its values as well as its originals, so a lone word would match other
// code by chance twice as often: the words around a flip go in as guards); a condition may not
// read a word the file writes (a toggle never switches its own condition or another's). Meant for
// branch flips in the game's code while an online mode is on (the match block's header says casual
// or ranked); real code goes in a cave.
namespace Orca::UX
{
class GuestMemory;  // NameTags.h

// One `when` line: (the word at `address` & `mask`) == `value`, or != when `equal` is false. An
// address that isn't mapped RAM never holds.
struct PatchCondition
{
  u32 address = 0;
  u32 mask = 0xFFFFFFFF;
  u32 value = 0;
  bool equal = true;

  bool operator==(const PatchCondition&) const = default;
};

struct GamePatch
{
  u32 address = 0;
  std::optional<u32> original;
  u32 value = 0;
  // Part of the previous line's group (both have originals, this one 4 bytes after it, and no
  // `when` or `end` between them).
  bool joins_previous = false;
  // A toggled line's conditions (its block's `when` lines, all must hold); empty for the others.
  std::vector<PatchCondition> when;
};

// The file's lines; nullopt and a reason for the first malformed one or a repeated address.
std::optional<std::vector<GamePatch>> ParseGamePatches(std::string_view text, std::string* error);

// Whether one group (or one `*` line) is written, given what memory holds at its addresses now.
// For a toggled group: whether it would land with its conditions holding.
bool GroupApplies(std::span<const GamePatch> group, std::span<const u32> now);

// Whether every condition holds in `memory` (true for none).
bool ConditionsHold(std::span<const PatchCondition> when, const GuestMemory& memory);

// What one toggled group does at a boundary, given whether its conditions hold and what memory
// holds at its addresses now (fewer words than the group: not all of it is mapped).
enum class Toggle
{
  Leave,  // already as the conditions want, or not the group's memory: nothing written
  On,     // every word is its original and the conditions hold: write the values
  Off,    // every word is its value and the conditions don't hold: write the originals back
};
Toggle ToggleGroup(std::span<const GamePatch> group, std::span<const u32> now, bool holds);

// One word to write at a boundary.
struct PatchWrite
{
  u32 address = 0;
  u32 value = 0;
  // Through Dolphin's memory-patch path, which drops the JIT's copy of the word: every guarded
  // line's word (toggled ones both ways). False: a `*` line's data, a plain write.
  bool code = false;
};

// One boundary's writes, only the words that change, and the groups they belong to (for the log).
struct PatchPlan
{
  struct Group
  {
    u32 address = 0;  // its first line's
    u32 words = 0;
    bool operator==(const Group&) const = default;
  };
  std::vector<PatchWrite> writes;
  std::vector<Group> landed;  // guarded groups that land
  std::vector<Group> on;      // toggled groups switched on
  std::vector<Group> off;     // toggled groups switched back off
};

// What the patches write given emulated memory now: the groups that land once and the `*` lines
// (`toggled` false), or the toggled groups (`toggled` true). Reads only.
PatchPlan PlanGamePatches(std::span<const GamePatch> patches, const GuestMemory& memory,
                          bool toggled);

// The active profile's patches (loaded when the profile changes); nothing without a profile or a
// file. A malformed file is a session error (Orca::Status "profile") and is not applied. At each
// frame boundary from the frame hook: ApplyGamePatches first (the groups that land once, the `*`
// lines), ApplyToggledGamePatches last, after every other writer.
void ApplyGamePatches(const Core::CPUThreadGuard& guard);
void ApplyToggledGamePatches(const Core::CPUThreadGuard& guard);
}  // namespace Orca::UX
