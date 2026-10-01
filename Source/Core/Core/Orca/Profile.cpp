// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Profile.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>

#include <fmt/format.h>

#include "Common/CPUDetect.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
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
#include "Core/PowerPC/PowerPC.h"

namespace Orca
{
namespace
{
bool EnvIs(const char* name, const char* value)
{
  const char* v = std::getenv(name);
  return v && std::string(v) == value;
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
  // CPU and timing. Same JIT on both machines: matchmaking pairs players on the same platform.
  f(Config::MAIN_CPU_THREAD, false);
  f(Config::MAIN_CPU_CORE, PowerPC::DefaultCPUCore());
  f(Config::MAIN_OVERCLOCK_ENABLE, false);
  f(Config::MAIN_OVERCLOCK, 1.0f);
  f(Config::MAIN_VI_OVERCLOCK_ENABLE, false);
  f(Config::MAIN_MMU, false);
  f(Config::MAIN_FASTMEM, Config::MAIN_FASTMEM.GetDefaultValue());
  f(Config::MAIN_ACCURATE_NANS, false);
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
  f(Config::MAIN_CUSTOM_RTC_ENABLE, true);
  f(Config::MAIN_CUSTOM_RTC_VALUE, Config::MAIN_CUSTOM_RTC_VALUE.GetDefaultValue());
  f(Config::MAIN_DSP_HLE, true);
  f(Config::MAIN_FAST_DISC_SPEED, false);
  f(Config::MAIN_SKIP_IPL, true);
  f(Config::SESSION_LOAD_IPL_DUMP, false);
  f(Config::MAIN_ENABLE_CHEATS, false);
  f(Config::MAIN_WII_SD_CARD, false);
  f(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED, false);
  f(Config::MAIN_OVERRIDE_REGION_SETTINGS, true);
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

  // Controllers: GameCube pads only. 1v1 by default.
  const bool four = TestPads() == 4;
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
  }

  void Save(Config::Layer*) override {}

private:
  Profile m_profile;
};
}  // namespace

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

bool TestOverridesActive()
{
  return TestPads() != 0 || TestSkipRender();
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
  std::vector<std::string> paths;
  const std::function<void(const File::FSTEntry&)> collect = [&](const File::FSTEntry& e) {
    if (!e.isDirectory)
      paths.push_back(e.physicalName);
    for (const auto& child : e.children)
      collect(child);
  };
  collect(tree);
  std::sort(paths.begin(), paths.end());
  u64 hash = 0xcbf29ce484222325ull;
  for (const std::string& path : paths)
  {
    std::string data;
    File::ReadFileToString(path, data);
    const std::string name = path.substr(dir.size());
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

std::optional<Profile> LoadProfile(const std::string& game_id, u16 revision, std::string* error)
{
  const std::string path = File::GetSysDirectory() + "Orca/" + game_id + ".ini";
  std::ifstream in(path);
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

    if (section == "Profile")
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
  if (*profile_revision != revision)
  {
    *error = fmt::format("This disc is {} revision {}; Orca's profile is for revision {}", game_id,
                         revision, *profile_revision);
    return std::nullopt;
  }
  profile.revision = *profile_revision;
  return profile;
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
