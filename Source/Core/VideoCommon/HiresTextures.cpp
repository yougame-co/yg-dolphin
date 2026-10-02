// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/HiresTextures.h"

#include <fmt/format.h>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <xxhash.h>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "Core/Orca/Profile.h"
#include "Core/System.h"
#include "VideoCommon/Assets/DirectFilesystemAssetLibrary.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Resources/CustomResourceManager.h"
#include "VideoCommon/VideoConfig.h"

constexpr std::string_view s_format_prefix{"tex1_"};

static std::unordered_map<std::string, std::shared_ptr<HiresTexture>> s_hires_texture_cache;
static std::unordered_map<std::string, bool> s_hires_texture_id_to_arbmipmap;

static auto s_file_library = std::make_shared<VideoCommon::DirectFilesystemAssetLibrary>();

namespace
{
std::pair<std::string, bool> GetNameArbPair(const TextureInfo& texture_info)
{
  if (s_hires_texture_id_to_arbmipmap.empty())
    return {"", false};

  const auto texture_name_details = texture_info.CalculateTextureName();
  // look for an exact match first
  const std::string full_name = texture_name_details.GetFullName();
  if (auto iter = s_hires_texture_id_to_arbmipmap.find(full_name);
      iter != s_hires_texture_id_to_arbmipmap.end())
  {
    return {full_name, iter->second};
  }

  // Single wildcard ignoring the tlut hash
  const std::string texture_name_single_wildcard_tlut =
      fmt::format("{}_{}_$_{}", texture_name_details.base_name, texture_name_details.texture_name,
                  texture_name_details.format_name);
  if (auto iter = s_hires_texture_id_to_arbmipmap.find(texture_name_single_wildcard_tlut);
      iter != s_hires_texture_id_to_arbmipmap.end())
  {
    return {texture_name_single_wildcard_tlut, iter->second};
  }

  // Single wildcard ignoring the texture hash
  const std::string texture_name_single_wildcard_tex =
      fmt::format("{}_${}_{}", texture_name_details.base_name, texture_name_details.tlut_name,
                  texture_name_details.format_name);
  if (auto iter = s_hires_texture_id_to_arbmipmap.find(texture_name_single_wildcard_tex);
      iter != s_hires_texture_id_to_arbmipmap.end())
  {
    return {texture_name_single_wildcard_tex, iter->second};
  }

  return {"", false};
}
}  // namespace

void HiresTexture::Shutdown()
{
  Clear();
}

void HiresTexture::Update()
{
  if (!g_ActiveConfig.bHiresTextures)
  {
    Clear();
    return;
  }

  const std::vector<std::string> game_ids = SConfig::GetInstance().GetGameIDsForTextures();
  const std::set<std::string> user_directories =
      GetTextureDirectoriesForFirstMatchingGameId(File::GetUserPath(D_HIRESTEXTURES_IDX), game_ids);
  std::vector<std::string> texture_directories(user_directories.begin(), user_directories.end());
  // Orca: a session also loads Orca's own pack (Data/Sys/Orca/Textures/<game ID>, the Online
  // menu's labels; ORCA.md "Online menu"), after the player's: the first file of a name wins, so a
  // player's own pack is never shadowed by ours. Replacing textures only changes what is drawn,
  // never emulated memory. Ours is always preloaded (a few small labels), so the game's own
  // label never shows for a frame while ours loads; the player's follows the player's setting.
  const size_t orca_pack_from = texture_directories.size();
  if (Orca::SessionActive())
  {
    for (const std::string& directory : GetTextureDirectoriesForFirstMatchingGameId(
             File::GetSysDirectory() + "Orca/Textures/", game_ids))
    {
      texture_directories.push_back(directory);
    }
  }

  constexpr auto extensions = std::to_array<std::string_view>({".png", ".dds"});

  for (size_t directory_index = 0; directory_index < texture_directories.size(); ++directory_index)
  {
    const std::string& texture_directory = texture_directories[directory_index];
    const bool preload = g_ActiveConfig.bCacheHiresTextures || directory_index >= orca_pack_from;
    // Watch this directory for any texture reloads
    s_file_library->Watch(texture_directory);

    const auto texture_paths =
        Common::DoFileSearch(texture_directory, extensions, /*recursive*/ true);

    bool failed_insert = false;
    for (auto& path : texture_paths)
    {
      std::string filename;
      SplitPath(path, nullptr, &filename, nullptr);

      if (filename.substr(0, s_format_prefix.length()) == s_format_prefix)
      {
        const size_t arb_index = filename.rfind("_arb");
        const bool has_arbitrary_mipmaps = arb_index != std::string::npos;
        if (has_arbitrary_mipmaps)
          filename.erase(arb_index, 4);

        const auto [it, inserted] =
            s_hires_texture_id_to_arbmipmap.try_emplace(filename, has_arbitrary_mipmaps);
        if (!inserted)
        {
          failed_insert = true;
        }
        else
        {
          // Since this is just a texture (single file) the mapper doesn't really matter
          // just provide a string
          s_file_library->SetAssetIDMapData(filename, std::map<std::string, std::filesystem::path>{
                                                          {"texture", StringToPath(path)}});

          if (preload)
          {
            auto hires_texture =
                std::make_shared<HiresTexture>(has_arbitrary_mipmaps, std::move(filename));
            static_cast<void>(hires_texture->LoadTexture());
            s_hires_texture_cache.try_emplace(hires_texture->GetId(), hires_texture);
          }
        }
      }
    }

    if (failed_insert)
    {
      ERROR_LOG_FMT(VIDEO, "One or more textures at path '{}' were already inserted",
                    texture_directory);
    }
  }

  const std::vector<std::string> game_ids_for_textures =
      SConfig::GetInstance().GetGameIDsForTextures();
  const std::string game_id_display = fmt::format("{}", fmt::join(game_ids_for_textures, "' or '"));

  std::string message;
  if (g_ActiveConfig.bCacheHiresTextures)
  {
    message = fmt::format("Preloading '{}' custom textures for '{}'", s_hires_texture_cache.size(),
                          game_id_display);
  }
  else
  {
    message = fmt::format("Found '{}' custom textures for '{}'",
                          s_hires_texture_id_to_arbmipmap.size(), game_id_display);
  }
  // Orca: a session's top left is the online overlay's (the connection circle and ping); Orca's
  // own pack is always there, so the count says nothing to the player.
  if (Orca::SessionActive())
    INFO_LOG_FMT(VIDEO, "{}", message);
  else
    OSD::AddMessage(message, 10000);
}

void HiresTexture::Clear()
{
  s_hires_texture_cache.clear();
  s_hires_texture_id_to_arbmipmap.clear();
  s_file_library = std::make_shared<VideoCommon::DirectFilesystemAssetLibrary>();
}

std::shared_ptr<HiresTexture> HiresTexture::Search(const TextureInfo& texture_info)
{
  auto [base_filename, has_arb_mipmaps] = GetNameArbPair(texture_info);
  if (base_filename == "")
    return nullptr;

  if (auto iter = s_hires_texture_cache.find(base_filename); iter != s_hires_texture_cache.end())
  {
    return iter->second;
  }
  else
  {
    auto hires_texture = std::make_shared<HiresTexture>(has_arb_mipmaps, std::move(base_filename));
    if (g_ActiveConfig.bCacheHiresTextures)
    {
      s_hires_texture_cache.try_emplace(hires_texture->GetId(), hires_texture);
    }
    return hires_texture;
  }
}

HiresTexture::HiresTexture(bool has_arbitrary_mipmaps, std::string id)
    : m_has_arbitrary_mipmaps(has_arbitrary_mipmaps), m_id(std::move(id))
{
}

VideoCommon::TextureDataResource* HiresTexture::LoadTexture() const
{
  auto& system = Core::System::GetInstance();
  auto& custom_resource_manager = system.GetCustomResourceManager();
  return custom_resource_manager.GetTextureDataFromAsset(m_id, s_file_library);
}

std::set<std::string> GetTextureDirectoriesWithGameId(const std::string& root_directory,
                                                      const std::string& game_id)
{
  std::set<std::string> result;
  const std::string texture_directory = root_directory + game_id;

  if (File::Exists(texture_directory))
  {
    result.insert(texture_directory);
  }
  else
  {
    // If there's no directory with the region-specific ID, look for a 3-character region-free one
    const std::string region_free_directory = root_directory + game_id.substr(0, 3);

    if (File::Exists(region_free_directory))
    {
      result.insert(region_free_directory);
    }
  }

  const auto match_gameid_or_all = [game_id](const std::string& filename) {
    std::string basename;
    SplitPath(filename, nullptr, &basename, nullptr);
    return basename == game_id || basename == game_id.substr(0, 3) || basename == "all";
  };

  // Look for any other directories that might be specific to the given gameid
  const auto files = Common::DoFileSearch(root_directory, ".txt", true);
  for (const auto& file : files)
  {
    if (match_gameid_or_all(file))
    {
      // The following code is used to calculate the top directory
      // of a found gameid.txt file
      // ex:  <root directory>/My folder/gameids/<gameid>.txt
      // would insert "<root directory>/My folder"
      const auto directory_path = file.substr(root_directory.size());
      const std::size_t first_path_separator_position = directory_path.find_first_of(DIR_SEP_CHR);
      result.insert(root_directory + directory_path.substr(0, first_path_separator_position));
    }
  }

  return result;
}

std::set<std::string>
GetTextureDirectoriesForFirstMatchingGameId(const std::string& root_directory,
                                            const std::vector<std::string>& game_ids)
{
  for (const auto& game_id : game_ids)
  {
    auto directories = GetTextureDirectoriesWithGameId(root_directory, game_id);
    if (!directories.empty())
      return directories;
  }
  return {};
}
