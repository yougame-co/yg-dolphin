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

struct Sram;

// Orca per-game profiles and the settings every Orca rollback session forces. Both players of a
// session must run identical emulated machines, so every setting that can change what the game sees
// is pinned: the universal ones in code (ForcedConfigLoader), the game-specific ones in
// Data/Sys/Orca/<GameID>.ini, which also names the disc revision and the frame boundary.
namespace Orca
{
struct Profile
{
  std::string game_id;
  u16 revision = 0;
  std::optional<u32> frame_hook;
  // Instruction word expected at frame_hook, checked before the hook is installed.
  std::optional<u32> frame_hook_word;
  // Game-specific additions to the universal settings, as <System>.<Section>.<Key> = <Value>.
  std::vector<std::pair<Config::Location, std::string>> forced;
};

// True when this process runs an Orca rollback session (ORCA_SESSION=1 for now; the desktop app
// will pass it at launch).
bool SessionActive();

// Test-only overrides for harness runs, honoured only inside a session. A process with any of them
// active must never offer or join a lobby, and its compatibility key differs.
//   ORCA_TEST_PADS=4         ports 3 and 4 connected too (4-player desync coverage)
//   ORCA_TEST_SKIP_RENDER=1  host rendering skipped for the whole run
//   ORCA_TEST_NET_DELAY_MS=N the YouGame room holds every message N ms (0-1000) each way, plus
//   ORCA_TEST_NET_JITTER_MS=J a deterministic 0..J ms more per message, order kept. Set it (to 0
//                            on the undelayed side) in both processes, so their keys match.
int TestPads();
bool TestSkipRender();
std::optional<int> TestNetDelayMs();
int TestNetJitterMs();
bool TestOverridesActive();

// Loads the profile for a game; nullopt and a reason when there is none, the revision differs or a
// line is malformed.
std::optional<Profile> LoadProfile(const std::string& game_id, u16 revision, std::string* error);

// The running session's profile, set at boot (null outside a session). The rollback core reads its
// frame boundary from it.
void SetActiveProfile(std::optional<Profile> profile);
const Profile* ActiveProfile();

// A config layer forcing the universal session settings plus the profile's (the Netplay layer type:
// Orca does not run Dolphin's own netplay, and refuses to start a session while it does).
std::unique_ptr<Config::ConfigLayerLoader> GenerateConfigLoader(const Profile& profile);

// The SRAM every session boots with (Dolphin's built-in default), instead of each player's own.
const Sram* SessionSram();

// A hash of the save the session NAND was seeded with (0 when there was none). Both players must
// match; the lobby compatibility key will include it.
u64 HashSessionSave(u64 title_id);
void SetSeedSaveHash(u64 hash);
u64 GetSeedSaveHash();

// After every other boot-time config change: an error if anything outranks a forced setting.
std::optional<std::string> VerifyForcedSettings(const Profile& profile);

// One line naming the effective values of the main forced settings, for the boot log.
std::string DescribeForcedSettings(const Profile& profile);
}  // namespace Orca
