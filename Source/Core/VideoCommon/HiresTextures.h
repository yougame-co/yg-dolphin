// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <fmt/ranges.h>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "VideoCommon/TextureInfo.h"

namespace VideoCommon
{
class TextureDataResource;
}

enum class TextureFormat;

std::set<std::string> GetTextureDirectoriesWithGameId(const std::string& root_directory,
                                                      const std::string& game_id);

// Tries each id in priority order, returning the directories for the first id that matches
// anything. Returns an empty set if none of the ids match.
std::set<std::string>
GetTextureDirectoriesForFirstMatchingGameId(const std::string& root_directory,
                                            const std::vector<std::string>& game_ids);

// One replacement file HiresTexture::Update registers.
struct HiresTextureFile
{
  std::string id;  // the texture's name, without "_arb"
  std::string path;
  bool has_arbitrary_mipmaps = false;
  bool orca_pack = false;  // from Orca's own pack (Data/Sys/Orca/Textures)
};

// Orca: the replacement files of `player_directories` (the player's own packs,
// <user>/Load/Textures) and `orca_directories` (Orca's pack, given only in a session), one per
// texture name. Orca's pack keeps the names it has, so no pack of the player's can bring back a
// texture Orca replaces (the Online menu's labels: nothing about connecting may look like
// Nintendo's, ORCA.md "Online menu"); the player's has every other name. Within each, the first
// directory and file of a name win, as in Dolphin. A mip level file (<name>_mip<N>) is not a
// texture of its own: it loads with its <name>.
std::vector<HiresTextureFile>
CollectHiresTextureFiles(const std::vector<std::string>& player_directories,
                         const std::vector<std::string>& orca_directories);

class HiresTexture
{
public:
  static void Update();
  static void Clear();
  static void Shutdown();
  static std::shared_ptr<HiresTexture> Search(const TextureInfo& texture_info);

  HiresTexture(bool has_arbitrary_mipmaps, std::string id);

  bool HasArbitraryMipmaps() const { return m_has_arbitrary_mipmaps; }
  VideoCommon::TextureDataResource* LoadTexture() const;
  const std::string& GetId() const { return m_id; }

private:
  bool m_has_arbitrary_mipmaps = false;
  std::string m_id;
};
