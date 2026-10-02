// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/GamePatches.h"

#include <cstdlib>
#include <mutex>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/Orca/Profile.h"
#include "Core/PowerPC/MMU.h"

namespace Orca::UX
{
namespace
{
std::optional<u32> Hex32(std::string_view word)
{
  if (word.empty() || word.size() > 8)
    return std::nullopt;
  u32 v = 0;
  for (const char c : word)
  {
    int d;
    if (c >= '0' && c <= '9')
      d = c - '0';
    else if (c >= 'a' && c <= 'f')
      d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      d = c - 'A' + 10;
    else
      return std::nullopt;
    v = v << 4 | static_cast<u32>(d);
  }
  return v;
}

std::vector<std::string_view> Words(std::string_view line)
{
  std::vector<std::string_view> words;
  size_t i = 0;
  while (i < line.size())
  {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
      ++i;
    const size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t')
      ++i;
    if (i > start)
      words.push_back(line.substr(start, i - start));
  }
  return words;
}
}  // namespace

std::optional<std::vector<GamePatch>> ParseGamePatches(std::string_view text, std::string* error)
{
  std::vector<GamePatch> patches;
  int number = 0;
  while (!text.empty())
  {
    ++number;
    const size_t nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    if (const size_t hash = line.find('#'); hash != std::string_view::npos)
      line = line.substr(0, hash);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    const auto words = Words(line);
    if (words.empty())
      continue;
    const auto address = words.size() == 3 ? Hex32(words[0]) : std::nullopt;
    const auto value = words.size() == 3 ? Hex32(words[2]) : std::nullopt;
    const auto original = words.size() == 3 && words[1] != "*" ? Hex32(words[1]) : std::nullopt;
    // A word-aligned address in MEM1 or MEM2, and an original that is either `*` or hex.
    const bool in_ram = address && (*address % 4 == 0) &&
                        ((*address >= 0x80000000 && *address < 0x81800000) ||
                         (*address >= 0x90000000 && *address < 0x94000000));
    if (!in_ram || !value || (words[1] != "*" && !original))
    {
      if (error)
        *error = fmt::format("line {}: expected `<address> <original|*> <value>` in hex", number);
      return std::nullopt;
    }
    patches.push_back({*address, original, *value});
  }
  return patches;
}

void ApplyGamePatches(const Core::CPUThreadGuard& guard)
{
  static std::once_flag s_loaded;
  static std::vector<GamePatch> s_patches;
  std::call_once(s_loaded, [] {
    const Orca::Profile* profile = Orca::ActiveProfile();
    if (!profile || profile->IsLauncher())
      return;
    const std::string path = File::GetSysDirectory() + "Orca/" + profile->game_id + ".patches";
    std::string text;
    if (!File::ReadFileToString(path, text))
      return;
    std::string error;
    if (auto parsed = ParseGamePatches(text, &error))
    {
      s_patches = std::move(*parsed);
      NOTICE_LOG_FMT(ROLLBACK, "Orca: {} game patches from {}", s_patches.size(), path);
    }
    else
    {
      ERROR_LOG_FMT(ROLLBACK, "Orca: {} refused, {}: no game patches", path, error);
    }
  });
  for (const GamePatch& p : s_patches)
  {
    if (!PowerPC::MMU::HostIsRAMAddress(guard, p.address))
      continue;
    const u32 now = PowerPC::MMU::HostRead<u32>(guard, p.address);
    if (now == p.value || (p.original && now != *p.original))
      continue;
    ApplyMemoryPatch<u32>(guard, p.value, p.address);
  }
}
}  // namespace Orca::UX
