// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"

// Orca per-game profiles: Data/Sys/Orca/<GameID>.ini. A profile names the disc revision it was made
// for, the frame boundary, and the settings both players of a rollback session are forced to, so
// their emulated machines stay identical.
namespace Orca
{
struct Profile
{
  std::string game_id;
  u16 revision = 0;
  std::optional<u32> frame_hook;
  // Instruction word expected at frame_hook, checked before the hook is installed.
  std::optional<u32> frame_hook_word;
  std::vector<std::pair<Config::Location, std::string>> forced;
};

// True when this process runs an Orca rollback session (ORCA_SESSION=1 for now; the desktop app
// will pass it at launch).
bool SessionActive();

// Test-only overrides (harness runs). ORCA_TEST_PADS=4 adds ports 3 and 4. A process with any test
// override active must never offer or join a lobby, and its compatibility key differs.
int TestPads();
bool TestOverridesActive();

// Loads the profile for a game, or nullopt with a reason when there is none or the revision differs.
std::optional<Profile> LoadProfile(const std::string& game_id, u16 revision, std::string* error);

// A config layer that forces the profile's settings for the session (the Netplay layer: Orca does
// not use Dolphin's own netplay).
std::unique_ptr<Config::ConfigLayerLoader> GenerateConfigLoader(const Profile& profile);
}  // namespace Orca
