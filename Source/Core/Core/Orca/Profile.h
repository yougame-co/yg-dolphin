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
namespace Core
{
class CPUThreadGuard;
}

// Orca per-game profiles and the settings every Orca rollback session forces. Both players of a
// session must run identical emulated machines, so every setting that can change what the game sees
// is pinned: the universal ones in code (ForcedConfigLoader), the game-specific ones in
// Data/Sys/Orca/<GameID>.ini, which also names the disc revision and the frame boundary.
namespace Orca
{
struct Profile
{
  // The profile's name: the disc's game ID for a disc profile (RSBE01), the ORCA_PROFILE name for a
  // launcher profile (PPLUS32).
  std::string game_id;
  // The disc's revision (a launcher profile: the revision of the disc the launcher boots).
  u16 revision = 0;
  std::optional<u32> frame_hook;
  // Instruction word expected at frame_hook. A disc profile checks it before installing the hook;
  // a launcher profile installs the hook at boot (the game isn't in RAM yet) and the boundary
  // ignores every pass while the word there differs.
  std::optional<u32> frame_hook_word;
  // Drop-in: by this frame the game's boot has written every NAND file it writes whatever the
  // player does (Brawl's /tmp packs), so a joining friend makes them by running its own boot to here
  // and the keyframe carries only their hashes. Unset: the keyframe carries every file.
  std::optional<u32> boot_nand_frame;
  // Game-specific additions to the universal settings, as <System>.<Section>.<Key> = <Value>.
  std::vector<std::pair<Config::Location, std::string>> forced;

  // Launcher profiles (a mod that boots through its own loader program, such as Project+): the
  // session boots a loader executable with exactly this XXH3-128 hash (32 hex digits), and the loader
  // boots the disc named by `disc` (and `revision`) from the DVD drive (Dolphin's default disc).
  std::string launcher_hash;
  std::string disc;
  // The SD card image the loader reads its files from, next to the loader executable, and its
  // XXH3-128 hash. A session opens it read-only.
  std::string sd_image;
  std::string sd_image_hash;
  // Set at boot: the verified image's full path. The session's config layer forces the SD card
  // path to it (not part of the lobby key: the hash is).
  std::string sd_image_path;
  // The title whose save seeds the session NAND (default: the booted title's own).
  std::optional<u64> save_title;
  // Set at boot: the game ID Dolphin gave what it booted (a loader's is derived from its file name).
  // Its game settings INIs apply while the loader runs, the disc's after it.
  std::string boot_game_id;

  bool IsLauncher() const { return !launcher_hash.empty(); }
};

// An environment variable as UTF-8, empty when unset. (On Windows std::getenv answers in the ANSI
// code page, which can't hold every character of a path or a name, and Dolphin's file functions
// expect UTF-8.)
std::string GetEnv(const char* name);

// True when this process runs an Orca rollback session: ORCA_SESSION=1, which Orca.app sets for
// itself unless the environment already says otherwise (ORCA_SESSION=0 turns it off).
bool SessionActive();

// Test-only overrides for harness runs, honoured only inside a session. A process with any of them
// active must never offer or join a lobby, and its compatibility key differs.
//   ORCA_TEST_PADS=4         ports 3 and 4 connected too (4-player desync coverage)
//   ORCA_TEST_SKIP_RENDER=1  host rendering skipped for the whole run
//   ORCA_TEST_NET_DELAY_MS=N the YouGame room holds every message N ms (0-1000) each way, plus
//   ORCA_TEST_NET_JITTER_MS=J a deterministic 0..J ms more per message, order kept. Set it (to 0
//                            on the undelayed side) in both processes, so their keys match.
//   ORCA_TEST_SNAPSHOT_EVERY=K  the session snapshots every K-th predicted frame (1-4) instead
//                            of choosing from the measured save time; the synctest
//                            (YG_SYNCTEST) then loads only frames that are multiples of K. Peers
//                            may use different K, but both need some test override for their
//                            keys to match.
//   ORCA_TEST_FULL_SNAPSHOTS=1  rollback snapshots copy all of RAM, as before copy-on-write
//                            snapshots (Rollback/Cow.h), to compare the two.
int TestPads();
bool TestSkipRender();
std::optional<int> TestNetDelayMs();
int TestNetJitterMs();
int TestSnapshotEvery();  // 0: unset
bool TestFullSnapshots();
bool TestOverridesActive();

// Loads the profile for a game; nullopt and a reason when there is none, the revision differs or a
// line is malformed. Without a revision (a launcher boot) the caller checks the disc itself.
std::optional<Profile> LoadProfile(const std::string& game_id, std::optional<u16> revision,
                                   std::string* error);

// ORCA_PROFILE: the launcher profile a session that boots an executable uses (empty when unset).
std::string LauncherProfileName();

// XXH3-128 of a file as 32 lowercase hex digits; empty when it can't be read.
std::string HashFile(const std::string& path);

// A launcher profile's boot checks, for a session booting the executable at `executable_path` with
// `disc_path` in the drive: the executable, the disc and the SD image must be the profile's. On
// success `profile->sd_image_path` names the verified image; on failure, `error` says why.
bool PrepareLauncherBoot(Profile* profile, const std::string& executable_path,
                         const std::string& disc_path, std::string* error);

// The running game's frame boundary is live: for a launcher profile, its instruction word is at the
// hook (a disc profile's hook was checked before it was installed).
bool FrameHookLive(const Core::CPUThreadGuard& guard, const Profile& profile);

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

// Whether a session copies the title's save from the configured NAND into the session NAND (on by
// default). Orca.app turns it off: until the canonical save exists (desktop plan O5), every app
// session starts from an empty NAND (save hash 0), whatever the player's own NAND holds, and the
// game makes its save in-session on both machines.
void SetSeedSessionSave(bool seed);
bool SeedSessionSave();

// After every other boot-time config change: an error if anything outranks a forced setting.
std::optional<std::string> VerifyForcedSettings(const Profile& profile);

// One line naming the effective values of the main forced settings, for the boot log.
std::string DescribeForcedSettings(const Profile& profile);
}  // namespace Orca
