// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/GamePatches.h"

#include <cstdlib>
#include <set>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"
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
  std::set<u32> seen;
  int number = 0;
  if (text.starts_with("\xEF\xBB\xBF"))  // a UTF-8 BOM an editor may add
    text.remove_prefix(3);
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
    if (!seen.insert(*address).second)
    {
      if (error)
        *error = fmt::format("line {}: address {:08X} repeated", number, *address);
      return std::nullopt;
    }
    const bool joins = original && !patches.empty() && patches.back().original &&
                       patches.back().address + 4 == *address;
    patches.push_back({*address, original, *value, joins});
  }
  return patches;
}

bool GroupApplies(std::span<const GamePatch> group, std::span<const u32> now)
{
  if (group.empty() || group.size() != now.size())
    return false;
  if (!group[0].original)  // a `*` line
    return now[0] != group[0].value;
  bool changes = false;
  for (size_t i = 0; i < group.size(); ++i)
  {
    if (now[i] != *group[i].original)
      return false;
    changes |= group[i].value != *group[i].original;
  }
  return changes;
}

void ApplyGamePatches(const Core::CPUThreadGuard& guard)
{
  // Loaded per profile (a process could boot a second game), on the CPU thread only.
  static std::string s_for;
  static std::vector<GamePatch> s_patches;
  // A disc profile's file (RSBE01.patches) or a launcher profile's (PPLUS32.patches). A launcher's
  // frame hook, which calls this, goes live only once the game it boots is in RAM.
  const Orca::Profile* profile = Orca::ActiveProfile();
  const std::string game = profile ? profile->game_id : std::string{};
  if (game != s_for)
  {
    s_for = game;
    s_patches.clear();
    const std::string path = File::GetSysDirectory() + "Orca/" + game + ".patches";
    std::string text;
    if (!game.empty() && File::ReadFileToString(path, text))
    {
      std::string error;
      if (auto parsed = ParseGamePatches(text, &error))
      {
        s_patches = std::move(*parsed);
        NOTICE_LOG_FMT(ROLLBACK, "Orca: {} game patches from {}", s_patches.size(), path);
      }
      else
      {
        ERROR_LOG_FMT(ROLLBACK, "Orca: {} refused, {}: no game patches", path, error);
        Orca::Status::Error("profile", "Orca's game data for this disc is damaged. Reinstall Orca.");
      }
    }
  }
  std::vector<u32> now;
  for (size_t start = 0; start < s_patches.size();)
  {
    size_t end = start + 1;
    while (end < s_patches.size() && s_patches[end].joins_previous)
      ++end;
    const std::span<const GamePatch> group(s_patches.data() + start, end - start);
    start = end;
    now.clear();
    for (const GamePatch& p : group)
    {
      if (!PowerPC::MMU::HostIsRAMAddress(guard, p.address))
        break;
      now.push_back(PowerPC::MMU::HostRead<u32>(guard, p.address));
    }
    if (!GroupApplies(group, now))
      continue;
    // A guarded group lands once per load of its module, and again on a re-run that crosses that
    // load (Tools/orca/online-menu.py checks where).
    if (group[0].original)
    {
      INFO_LOG_FMT(ROLLBACK, "Orca: patch group {:08x} ({} words) landed", group[0].address,
                   group.size());
    }
    for (size_t i = 0; i < group.size(); ++i)
    {
      if (now[i] == group[i].value)
        continue;
      if (group[i].original)
        ApplyMemoryPatch<u32>(guard, group[i].value, group[i].address);  // code: drop the JIT's copy
      else
        PowerPC::MMU::HostWrite<u32>(guard, group[i].value, group[i].address);
    }
  }
}
}  // namespace Orca::UX
