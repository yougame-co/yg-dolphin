// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Online.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>

#include <cctype>

#include <fmt/format.h>
#include <xxh3.h>

#include "Common/CPUDetect.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"
#ifdef USE_RETRO_ACHIEVEMENTS
#include "Core/Config/AchievementSettings.h"
#endif
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Sram.h"
#include "Core/HW/Wiimote.h"
#include "Core/Orca/Session/Session.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "DiscIO/Volume.h"

namespace Orca
{
namespace
{
bool EnvIs(const char* name, const char* value)
{
  return GetEnv(name) == value;
}

// Decimal, or hex with a 0x prefix (no octal: a leading zero is still decimal).
bool ParseNumber(const std::string& text, u32* out)
{
  if (text.starts_with("0x") || text.starts_with("0X"))
    return TryParse(text, out, 16);
  return TryParse(text, out, 10);
}

// Every setting an Orca session forces, whatever the game. Values are applied by ForcedConfigLoader
// and checked by VerifyForcedSettings, so the two lists can't drift apart.
template <typename F>
void ForEachUniversalSetting(F&& f)
{
  // CPU and timing. Each host's own JIT (JitArm64 on a Mac, Jit64 on a PC): the two compute the
  // same game (see MAIN_ACCURATE_NANS below and Online.cpp CompatibilityKey).
  f(Config::MAIN_CPU_THREAD, false);
  f(Config::MAIN_CPU_CORE, TestCpuCore().value_or(PowerPC::DefaultCPUCore()));
  f(Config::MAIN_OVERCLOCK_ENABLE, false);
  f(Config::MAIN_OVERCLOCK, 1.0f);
  f(Config::MAIN_VI_OVERCLOCK_ENABLE, false);
  f(Config::MAIN_MMU, false);
  f(Config::MAIN_FASTMEM, Config::MAIN_FASTMEM.GetDefaultValue());
  // PowerPC's NaN rules on every host: an input NaN is picked in PowerPC's operand order and made
  // quiet, and a NaN an operation makes from no NaN (inf * 0, inf - inf, 0 / 0, frsqrte of -1)
  // is the positive default 0x7FF8000000000000. The hosts' own rules differ: x86-64 and an ARM CPU
  // with FEAT_AFP (FPCR.AH, Apple M4 and later) make that NaN negative, an ARM CPU without it
  // (Apple M1 to M3) positive, and they pick among NaN inputs differently. Project+ makes one with
  // fnmsubs (inf * 0) as a match starts, so before 2026-10-02 an M1 Mac couldn't play it with an M4
  // Mac or a PC (Online.cpp kept those players apart then). Both JITs' accurate-NaN paths cost a
  // compare and a branch per arithmetic op that is never taken unless a NaN comes out.
  f(Config::MAIN_ACCURATE_NANS, true);
  f(Config::MAIN_ACCURATE_FMADDS, Config::MAIN_ACCURATE_FMADDS.GetDefaultValue());
  f(Config::MAIN_FPRF, false);
  f(Config::MAIN_FLOAT_EXCEPTIONS, false);
  f(Config::MAIN_DIVIDE_BY_ZERO_EXCEPTIONS, false);
  f(Config::MAIN_DISABLE_ICACHE, false);
  f(Config::MAIN_ACCURATE_CPU_CACHE, false);
  f(Config::MAIN_JIT_FOLLOW_BRANCH, Config::MAIN_JIT_FOLLOW_BRANCH.GetDefaultValue());
  f(Config::MAIN_SYNC_ON_SKIP_IDLE, Config::MAIN_SYNC_ON_SKIP_IDLE.GetDefaultValue());
  // Fused multiply-add changes float results. ARM64 always has it; on x86-64 it follows the host,
  // so it belongs in the lobby compatibility key (Phase 7).
  f(Config::SESSION_USE_FMA, cpu_info.bFMA);
  f(Config::MAIN_RAM_OVERRIDE_ENABLE, false);
  // Switching to the YouGame app (to invite a friend) must not pause the match.
  f(Config::MAIN_PAUSE_ON_FOCUS_LOST, false);
  // Orca talks to YouGame only: no usage reports to Dolphin's analytics server.
  f(Config::MAIN_ANALYTICS_ENABLED, false);
  f(Config::MAIN_CUSTOM_RTC_ENABLE, true);
  // A pinned clock, the same for both players, but not Dolphin's default (2000-01-01 00:00:00, the
  // GameCube epoch): from there the timebase reads about -2^32 at boot (Brawl rev 2's SI TypeTime),
  // and an SI alarm queued then for a small absolute time waits ~70 s for the timebase to climb
  // back past 0 (Brawl's GameCube pads stayed in PADReset until frame ~4200). 2025-01-01 UTC.
  f(Config::MAIN_CUSTOM_RTC_VALUE, 1735689600u);
  f(Config::MAIN_DSP_HLE, true);
  f(Config::MAIN_FAST_DISC_SPEED, false);
  f(Config::MAIN_SKIP_IPL, true);
  f(Config::SESSION_LOAD_IPL_DUMP, false);
  f(Config::MAIN_ENABLE_CHEATS, false);
  // No SD card unless the profile inserts one; a profile's card is a verified image, read-only
  // (writes would outlive a rollback and differ between players), never rebuilt from a folder.
  f(Config::MAIN_WII_SD_CARD, false);
  f(Config::MAIN_ALLOW_SD_WRITES, false);
  f(Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC, false);
  f(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED, false);
  f(Config::MAIN_OVERRIDE_REGION_SETTINGS, true);
  // The console region a DOL boots as (the Project+ launcher), which Dolphin otherwise takes from
  // the host's locale: a Mac it couldn't read a country from booted P+ as a PAL console
  // (setting.txt EUR/PAL) and a US PC as NTSC-U, and their games diverged from the first frame.
  // Orca's games are NTSC-U discs. A disc boot takes the disc's region and never reads this.
  f(Config::MAIN_FALLBACK_REGION, DiscIO::Region::NTSC_U);
  f(Config::MAIN_GC_LANGUAGE, 0);  // English; written into the session SRAM
  f(Config::MAIN_WII_WIILINK_ENABLE, false);
  f(Config::MAIN_EMULATE_SKYLANDER_PORTAL, false);
  f(Config::MAIN_EMULATE_INFINITY_BASE, false);
  f(Config::MAIN_EMULATE_WII_SPEAK, false);
  for (const auto& mic : Config::MAIN_EMULATE_LOGITECH_MIC)
    f(mic, false);
  // No memory cards or other EXI devices (netplay forces these too).
  for (const auto slot : {ExpansionInterface::Slot::A, ExpansionInterface::Slot::B,
                          ExpansionInterface::Slot::SP1})
  {
    f(Config::GetInfoForEXIDevice(slot), ExpansionInterface::EXIDeviceType::None);
  }
#ifdef USE_RETRO_ACHIEVEMENTS
  // Hardcore mode refuses state loads, which rollback needs.
  f(Config::RA_HARDCORE_ENABLED, false);
#endif

  // Wii system settings: the same on both machines rather than each user's.
  f(Config::SYSCONF_LANGUAGE, 1u);  // English
  f(Config::SYSCONF_COUNTRY, 49u);  // USA
  f(Config::SYSCONF_WIDESCREEN, true);
  f(Config::SYSCONF_PROGRESSIVE_SCAN, true);
  f(Config::SYSCONF_PAL60, false);  // BootCore forces it off for NTSC Wii games anyway
  f(Config::SYSCONF_SOUND_MODE, 1u);

  // Controllers: GameCube pads only. 1v1 by default; four ports in online (drop-in) play, where a
  // port without a player reports no controller (Orca::Net::UNPLUGGED_PAD) until a friend plugs in.
  const bool four = TestPads() == 4 || Online::Enabled();
  f(Config::GetInfoForSIDevice(0), SerialInterface::SIDEVICE_GC_CONTROLLER);
  f(Config::GetInfoForSIDevice(1), SerialInterface::SIDEVICE_GC_CONTROLLER);
  f(Config::GetInfoForSIDevice(2),
    four ? SerialInterface::SIDEVICE_GC_CONTROLLER : SerialInterface::SIDEVICE_NONE);
  f(Config::GetInfoForSIDevice(3),
    four ? SerialInterface::SIDEVICE_GC_CONTROLLER : SerialInterface::SIDEVICE_NONE);
  for (int i = 0; i < 5; ++i)
    f(Config::GetInfoForWiimoteSource(i), WiimoteSource::None);

  // Graphics: copies stay on the GPU so RAM only ever gets a constant placeholder, never pixels; no
  // CPU readbacks of GPU results; no graphics mods (they can skip copies).
  f(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, true);
  f(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM, true);
  f(Config::GFX_HACK_DISABLE_COPY_TO_VRAM, false);
  f(Config::GFX_HACK_DEFER_EFB_COPIES, true);
  f(Config::GFX_HACK_IMMEDIATE_XFB, false);
  f(Config::GFX_HACK_EFB_ACCESS_ENABLE, false);
  f(Config::GFX_HACK_BBOX_ENABLE, false);
  f(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, false);
  f(Config::GFX_PERF_QUERIES_ENABLE, false);
  f(Config::GFX_MODS_ENABLE, false);
}

class ForcedConfigLoader final : public Config::ConfigLayerLoader
{
public:
  explicit ForcedConfigLoader(Profile profile)
      : ConfigLayerLoader(Config::LayerType::Netplay), m_profile(std::move(profile))
  {
  }

  void Load(Config::Layer* layer) override
  {
    ForEachUniversalSetting([layer](const auto& info, auto value) { layer->Set(info, value); });
    for (const auto& [location, value] : m_profile.forced)
      layer->Set(location, value);
    // A launcher profile's verified SD image, over any card path the player set.
    if (!m_profile.sd_image_path.empty())
      layer->Set(Config::MAIN_WII_SD_CARD_IMAGE_PATH, m_profile.sd_image_path);
  }

  void Save(Config::Layer*) override {}

private:
  Profile m_profile;
};
}  // namespace

std::string GetEnv(const char* name)
{
#ifdef _WIN32
  const wchar_t* value = _wgetenv(UTF8ToWString(name).c_str());
  return value ? WStringToUTF8(value) : std::string();
#else
  const char* value = std::getenv(name);
  return value ? value : std::string();
#endif
}

bool SessionActive()
{
  return EnvIs("ORCA_SESSION", "1");
}

int TestPads()
{
  return SessionActive() && EnvIs("ORCA_TEST_PADS", "4") ? 4 : 0;
}

bool TestSkipRender()
{
  return SessionActive() && EnvIs("ORCA_TEST_SKIP_RENDER", "1");
}

std::optional<int> TestNetDelayMs()
{
  const std::string value = GetEnv("ORCA_TEST_NET_DELAY_MS");
  if (!SessionActive() || value.empty())
    return std::nullopt;
  return std::clamp(std::atoi(value.c_str()), 0, 1000);
}

int TestNetJitterMs()
{
  const std::string value = GetEnv("ORCA_TEST_NET_JITTER_MS");
  return TestNetDelayMs() && !value.empty() ? std::clamp(std::atoi(value.c_str()), 0, 1000) : 0;
}

int TestSnapshotEvery()
{
  const std::string value = GetEnv("ORCA_TEST_SNAPSHOT_EVERY");
  if (!SessionActive() || value.empty())
    return 0;
  return std::clamp(std::atoi(value.c_str()), 1, Net::MAX_SNAPSHOT_EVERY);
}

bool TestFullSnapshots()
{
  return SessionActive() && EnvIs("ORCA_TEST_FULL_SNAPSHOTS", "1");
}

std::optional<PowerPC::CPUCore> TestCpuCore()
{
  const std::string value = GetEnv("ORCA_TEST_CPUCORE");
  if (!SessionActive() || value.empty())
    return std::nullopt;
  switch (std::atoi(value.c_str()))
  {
  case static_cast<int>(PowerPC::CPUCore::Interpreter):
    return PowerPC::CPUCore::Interpreter;
  case static_cast<int>(PowerPC::CPUCore::CachedInterpreter):
    return PowerPC::CPUCore::CachedInterpreter;
  default:
    return std::nullopt;
  }
}

bool TestNoAfp()
{
  // Read where the CPU is detected (Common/ArmCPUDetect.cpp), which clears cpu_info.bAFP; only an
  // ARM build has anything to clear, but the key counts it everywhere.
  return EnvIs("ORCA_TEST_NO_AFP", "1");
}

bool TestOverridesActive()
{
  return TestPads() != 0 || TestSkipRender() || TestNetDelayMs().has_value() ||
         TestSnapshotEvery() != 0 || TestFullSnapshots() || TestNoAfp() ||
         TestCpuCore().has_value();
}

const Sram* SessionSram()
{
  static const Sram sram = [] {
    Sram s;
    InitSRAM(&s, "");  // no file: Dolphin's built-in default
    return s;
  }();
  return &sram;
}

namespace
{
u64 s_seed_save_hash = 0;
bool s_seed_session_save = true;
std::optional<Profile> s_active_profile;
}  // namespace

void SetActiveProfile(std::optional<Profile> profile)
{
  s_active_profile = std::move(profile);
}

const Profile* ActiveProfile()
{
  return s_active_profile ? &*s_active_profile : nullptr;
}

u64 HashSessionSave(u64 title_id)
{
  const std::string dir = File::GetUserPath(D_SESSION_WIIROOT_IDX) +
                          fmt::format("/title/{:08x}/{:08x}/data/", static_cast<u32>(title_id >> 32),
                                      static_cast<u32>(title_id));
  if (!File::IsDirectory(dir))
    return 0;
  // Files in name order, each hashed with its name.
  File::FSTEntry tree = File::ScanDirectoryTree(dir, true);
  // Names relative to the scanned root as ScanDirectoryTree spells it (on Windows it rebuilds paths
  // in generic form), so every platform hashes the same names.
  std::string root = tree.physicalName;
  if (!root.ends_with('/'))
    root += '/';
  std::vector<std::string> paths;
  const std::function<void(const File::FSTEntry&)> collect = [&](const File::FSTEntry& e) {
    if (!e.isDirectory)
      paths.push_back(e.physicalName);
    for (const auto& child : e.children)
      collect(child);
  };
  collect(tree);
  // The title's data folder exists from boot on (ES makes it), so an empty one means no save too.
  if (paths.empty())
    return 0;
  std::sort(paths.begin(), paths.end());
  u64 hash = 0xcbf29ce484222325ull;
  for (const std::string& path : paths)
  {
    std::string data;
    File::ReadFileToString(path, data);
    const std::string name = path.starts_with(root) ? path.substr(root.size()) : path;
    hash = (hash ^ Common::GetHash64(reinterpret_cast<const u8*>(name.data()),
                                     static_cast<u32>(name.size()), 0)) *
           0x100000001b3ull;
    hash = (hash ^ Common::GetHash64(reinterpret_cast<const u8*>(data.data()),
                                     static_cast<u32>(data.size()), 0)) *
           0x100000001b3ull;
  }
  return hash == 0 ? 1 : hash;
}

void SetSeedSaveHash(u64 hash)
{
  s_seed_save_hash = hash;
}

u64 GetSeedSaveHash()
{
  return s_seed_save_hash;
}

void SetSeedSessionSave(bool seed)
{
  s_seed_session_save = seed;
}

bool SeedSessionSave()
{
  return s_seed_session_save;
}

std::optional<Profile> LoadProfile(const std::string& game_id, std::optional<u16> revision,
                                   std::string* error)
{
  const std::string path = File::GetSysDirectory() + "Orca/" + game_id + ".ini";
  // Through OpenFStream: the path is UTF-8, which std::ifstream would read as the ANSI code page on
  // Windows (an install path with any non-ASCII character would find no profile).
  std::ifstream in;
  File::OpenFStream(in, path, std::ios_base::in);
  if (!in)
  {
    *error = fmt::format("Orca has no profile for {} ({})", game_id, path);
    return std::nullopt;
  }

  Profile profile;
  profile.game_id = game_id;
  std::optional<u16> profile_revision;
  std::string section, line;
  int line_number = 0;
  const auto fail = [&](std::string_view why) {
    *error = fmt::format("Orca profile {} line {}: {}", path, line_number, why);
    return std::nullopt;
  };
  while (std::getline(in, line))
  {
    ++line_number;
    // Comments start a line, or follow whitespace.
    if (const size_t hash = line.find('#');
        hash == 0 || (hash != std::string::npos && (line[hash - 1] == ' ' || line[hash - 1] == '\t')))
    {
      line.resize(hash);
    }
    line = std::string(StripWhitespace(line));
    if (line.empty())
      continue;
    if (line.front() == '[' && line.back() == ']')
    {
      section = line.substr(1, line.size() - 2);
      if (section != "Profile" && section != "Forced")
        return fail(fmt::format("unknown section [{}]", section));
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      return fail("expected <key> = <value>");
    const std::string key(StripWhitespace(line.substr(0, eq)));
    const std::string value(StripWhitespace(line.substr(eq + 1)));
    if (key.empty() || value.empty())
      return fail("empty key or value");

    if (section == "Profile" && (key == "Launcher" || key == "SDImageHash"))
    {
      if (value.size() != 32 ||
          !std::ranges::all_of(value, [](unsigned char c) {
            return std::isxdigit(c) && !std::isupper(c);
          }))
      {
        return fail(fmt::format("{} must be 32 lowercase hex digits (XXH3-128)", key));
      }
      (key == "Launcher" ? profile.launcher_hash : profile.sd_image_hash) = value;
    }
    else if (section == "Profile" && (key == "Disc" || key == "SDImage"))
    {
      // A file name next to the launcher, never a path out of its folder.
      if (key == "SDImage" && (value.find_first_of("/\\") != std::string::npos || value == ".."))
        return fail("SDImage must be a file name");
      (key == "Disc" ? profile.disc : profile.sd_image) = value;
    }
    else if (section == "Profile" && key == "Title")
    {
      // Shown in the window title: printable ASCII only, and short.
      if (value.size() > 48 ||
          !std::ranges::all_of(value, [](unsigned char c) { return c >= 0x20 && c < 0x7f; }))
      {
        return fail("Title must be at most 48 printable ASCII characters");
      }
      profile.title = value;
    }
    else if (section == "Profile" && key == "SaveTitle")
    {
      u64 title = 0;
      if (!value.starts_with("0x") || !TryParse(value.substr(2), &title, 16))
        return fail("SaveTitle must be a hex title ID (0x...)");
      profile.save_title = title;
    }
    else if (section == "Profile")
    {
      u32 number = 0;
      if (!ParseNumber(value, &number))
        return fail(fmt::format("{} is not a number", value));
      if (key == "Revision")
        profile_revision = static_cast<u16>(number);
      else if (key == "FrameHook")
        profile.frame_hook = number;
      else if (key == "FrameHookWord")
        profile.frame_hook_word = number;
      else if (key == "BootNandFrame")
        profile.boot_nand_frame = number;
      else
        return fail(fmt::format("unknown key {}", key));
    }
    else if (section == "Forced")
    {
      // <System>.<Section>.<Key>, as on the command line.
      const size_t dot1 = key.find('.');
      const size_t dot2 = dot1 == std::string::npos ? dot1 : key.find('.', dot1 + 1);
      if (dot2 == std::string::npos)
        return fail(fmt::format("{} is not <System>.<Section>.<Key>", key));
      const std::optional<Config::System> system = Config::GetSystemFromName(key.substr(0, dot1));
      if (!system)
        return fail(fmt::format("unknown system in {}", key));
      profile.forced.emplace_back(Config::Location{*system, key.substr(dot1 + 1, dot2 - dot1 - 1),
                                                   key.substr(dot2 + 1)},
                                  value);
    }
    else
    {
      return fail("setting outside a section");
    }
  }

  if (!profile_revision)
  {
    *error = fmt::format("Orca profile {} has no Revision", path);
    return std::nullopt;
  }
  if (revision && *profile_revision != *revision)
  {
    *error = fmt::format("This disc is {} revision {}; Orca's profile is for revision {}", game_id,
                         *revision, *profile_revision);
    return std::nullopt;
  }
  profile.revision = *profile_revision;
  // A launcher profile names the whole boot: loader, disc and SD image.
  const bool any_launcher = !profile.launcher_hash.empty() || !profile.disc.empty() ||
                            !profile.sd_image.empty() || !profile.sd_image_hash.empty();
  if (any_launcher && (profile.launcher_hash.empty() || profile.disc.empty() ||
                       profile.sd_image.empty() != profile.sd_image_hash.empty() ||
                       !profile.frame_hook || !profile.frame_hook_word))
  {
    // Without the word, the boundary would fire inside the loader.
    *error = fmt::format("Orca profile {}: a launcher profile needs Launcher, Disc, FrameHook and "
                         "FrameHookWord, and SDImage with SDImageHash",
                         path);
    return std::nullopt;
  }
  return profile;
}

std::string LauncherProfileName()
{
  const char* name = std::getenv("ORCA_PROFILE");
  if (!name)
    return {};
  // A profile file name in Data/Sys/Orca, nothing else.
  const std::string value(name);
  if (value.empty() || value.size() > 32 ||
      !std::ranges::all_of(value,
                           [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; }))
  {
    return {};
  }
  return value;
}

std::string HashFile(const std::string& path)
{
  File::IOFile file(path, "rb");
  if (!file)
    return {};
  XXH3_state_t* state = XXH3_createState();
  if (!state)
    return {};
  XXH3_128bits_reset(state);
  std::vector<u8> buffer(8 << 20);
  u64 remaining = file.GetSize();
  bool ok = true;
  while (remaining > 0)
  {
    const size_t n = static_cast<size_t>(std::min<u64>(remaining, buffer.size()));
    if (!file.ReadBytes(buffer.data(), n))
    {
      ok = false;
      break;
    }
    XXH3_128bits_update(state, buffer.data(), n);
    remaining -= n;
  }
  const XXH128_hash_t hash = XXH3_128bits_digest(state);
  XXH3_freeState(state);
  return ok ? fmt::format("{:016x}{:016x}", hash.high64, hash.low64) : std::string{};
}

bool PrepareLauncherBoot(Profile* profile, const std::string& executable_path,
                         const std::string& disc_path, std::string* error)
{
  if (HashFile(executable_path) != profile->launcher_hash)
  {
    *error = fmt::format("Orca: {} is not the loader profile {} names", executable_path,
                         profile->game_id);
    return false;
  }

  const std::unique_ptr<DiscIO::Volume> disc =
      disc_path.empty() ? nullptr : DiscIO::CreateVolume(disc_path);
  if (!disc)
  {
    *error = fmt::format("Orca: profile {} boots {} from the drive, and no disc is set",
                         profile->game_id, profile->disc);
    return false;
  }
  const u16 disc_revision = disc->GetRevision().value_or(0);
  if (disc->GetGameID() != profile->disc || disc_revision != profile->revision)
  {
    *error = fmt::format("Orca: profile {} needs {} revision {}; the disc is {} revision {}",
                         profile->game_id, profile->disc, profile->revision, disc->GetGameID(),
                         disc_revision);
    return false;
  }

  if (profile->sd_image.empty())
    return true;
  std::string folder;
  SplitPath(executable_path, &folder, nullptr, nullptr);
  const std::string image = folder + profile->sd_image;
  if (HashFile(image) != profile->sd_image_hash)
  {
    *error = fmt::format("Orca: {} is missing or is not the SD card profile {} names", image,
                         profile->game_id);
    return false;
  }
  // The session's config layer points the SD card at this image (read-only: writes are forced
  // off), never at the player's own card.
  profile->sd_image_path = image;
  return true;
}

bool FrameHookLive(const Core::CPUThreadGuard& guard, const Profile& profile)
{
  if (!profile.IsLauncher() || !profile.frame_hook || !profile.frame_hook_word)
    return true;
  return PowerPC::MMU::HostRead<u32>(guard, *profile.frame_hook) == *profile.frame_hook_word;
}

std::unique_ptr<Config::ConfigLayerLoader> GenerateConfigLoader(const Profile& profile)
{
  return std::make_unique<ForcedConfigLoader>(profile);
}

std::optional<std::string> VerifyForcedSettings(const Profile& profile)
{
  std::vector<Config::Location> locations;
  ForEachUniversalSetting(
      [&locations](const auto& info, auto) { locations.push_back(info.GetLocation()); });
  for (const auto& [location, value] : profile.forced)
    locations.push_back(location);
  if (!profile.sd_image_path.empty())
    locations.push_back(Config::MAIN_WII_SD_CARD_IMAGE_PATH.GetLocation());

  for (const Config::Location& location : locations)
  {
    const Config::LayerType active = Config::GetActiveLayerForConfig(location);
    if (active != Config::LayerType::Netplay)
    {
      return fmt::format("Orca: {}.{}.{} is overridden by another config layer ({})",
                         Config::GetSystemName(location.system), location.section, location.key,
                         static_cast<int>(active));
    }
  }
  return std::nullopt;
}

std::string DescribeForcedSettings(const Profile& profile)
{
  return fmt::format("Orca: profile {} rev {}{}: CPUThread={} CPUCore={} FMA={} RTC={}:{} "
                     "SI={},{},{},{} EFBToTexture={} XFBToTexture={} EFBAccess={} BBox={}",
                     profile.game_id, profile.revision,
                     TestOverridesActive() ? " (TEST overrides active, not for real sessions)" : "",
                     Config::Get(Config::MAIN_CPU_THREAD),
                     static_cast<int>(Config::Get(Config::MAIN_CPU_CORE)),
                     Config::Get(Config::SESSION_USE_FMA), Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE),
                     Config::Get(Config::MAIN_CUSTOM_RTC_VALUE),
                     static_cast<int>(Config::Get(Config::GetInfoForSIDevice(0))),
                     static_cast<int>(Config::Get(Config::GetInfoForSIDevice(1))),
                     static_cast<int>(Config::Get(Config::GetInfoForSIDevice(2))),
                     static_cast<int>(Config::Get(Config::GetInfoForSIDevice(3))),
                     Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM),
                     Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM),
                     Config::Get(Config::GFX_HACK_EFB_ACCESS_ENABLE),
                     Config::Get(Config::GFX_HACK_BBOX_ENABLE));
}
}  // namespace Orca
