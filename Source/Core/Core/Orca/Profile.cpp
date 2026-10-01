// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Profile.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/PowerPC/PowerPC.h"

namespace Orca
{
bool SessionActive()
{
  const char* v = std::getenv("ORCA_SESSION");
  return v && *v == '1';
}

int TestPads()
{
  const char* v = std::getenv("ORCA_TEST_PADS");
  return v && std::atoi(v) == 4 ? 4 : 0;
}

bool TestOverridesActive()
{
  return TestPads() != 0;
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
  while (std::getline(in, line))
  {
    line = std::string(StripWhitespace(line));
    if (line.empty() || line[0] == '#' || line[0] == ';')
      continue;
    if (line.front() == '[' && line.back() == ']')
    {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key(StripWhitespace(line.substr(0, eq)));
    const std::string value(StripWhitespace(line.substr(eq + 1)));

    if (section == "Profile")
    {
      u32 number = 0;
      if (!TryParse(value, &number))
        continue;
      if (key == "Revision")
        profile_revision = static_cast<u16>(number);
      else if (key == "FrameHook")
        profile.frame_hook = number;
      else if (key == "FrameHookWord")
        profile.frame_hook_word = number;
    }
    else if (section == "Forced")
    {
      // <System>.<Section>.<Key>, as on the command line.
      const size_t dot1 = key.find('.');
      const size_t dot2 = dot1 == std::string::npos ? dot1 : key.find('.', dot1 + 1);
      if (dot2 == std::string::npos)
        continue;
      const std::optional<Config::System> system = Config::GetSystemFromName(key.substr(0, dot1));
      if (!system)
        continue;
      profile.forced.emplace_back(Config::Location{*system, key.substr(dot1 + 1, dot2 - dot1 - 1),
                                                   key.substr(dot2 + 1)},
                                  value);
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

namespace
{
class OrcaConfigLayerLoader final : public Config::ConfigLayerLoader
{
public:
  explicit OrcaConfigLayerLoader(Profile profile)
      : ConfigLayerLoader(Config::LayerType::Netplay), m_profile(std::move(profile))
  {
  }

  void Load(Config::Layer* layer) override
  {
    for (const auto& [location, value] : m_profile.forced)
      layer->Set(location, value);
    // Same JIT on both machines (matchmaking pairs players on the same platform).
    layer->Set(Config::MAIN_CPU_CORE, PowerPC::DefaultCPUCore());
    // GameCube controllers only.
    for (int i = 0; i < 5; ++i)
      layer->Set(Config::GetInfoForWiimoteSource(i), WiimoteSource::None);
    // Test harness only: ORCA_TEST_PADS=4 connects ports 3 and 4 too, for 4-player desync coverage.
    // Never set by the desktop app; a session with it must not join a lobby (TestOverridesActive).
    if (TestPads() == 4)
    {
      layer->Set(Config::GetInfoForSIDevice(2), SerialInterface::SIDEVICE_GC_CONTROLLER);
      layer->Set(Config::GetInfoForSIDevice(3), SerialInterface::SIDEVICE_GC_CONTROLLER);
    }
  }

  void Save(Config::Layer*) override {}

private:
  Profile m_profile;
};
}  // namespace

std::unique_ptr<Config::ConfigLayerLoader> GenerateConfigLoader(const Profile& profile)
{
  return std::make_unique<OrcaConfigLayerLoader>(profile);
}
}  // namespace Orca
