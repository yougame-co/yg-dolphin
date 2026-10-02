// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/NameTags.h"

#include <cstdlib>

#include "Core/Core.h"
#include "Core/PowerPC/MMU.h"

namespace Orca::UX
{
namespace
{
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 GAME_GLOBAL = 0x805A00E0;
constexpr u32 GAME_GLOBAL_NAME_RECORDS = 0x28;
constexpr u32 RECORDS_TAGS = 0xE0;
constexpr u32 TAG_SIZE = 0x124;
constexpr u32 SCENE_SELCHAR_TASK = 0x400;
constexpr u32 TASK_AREAS = 0x44;
constexpr u32 AREA_INDEX = 0x1B0;
constexpr u32 AREA_PLAYER_KIND = 0x1B4;
constexpr u32 AREA_NAME_ID = 0x1C8;
constexpr u32 PLAYER_KIND_HUMAN = 1;
constexpr u32 NO_TAG = 0xFFFFFFFF;
constexpr char CHARACTER_SELECT[] = "scSelctCharacter";

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

bool SceneIs(const GuestMemory& m, std::string_view want)
{
  if (!Pointer(m, SCENE_MANAGER))
    return false;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + 4))
    return false;
  const u32 scene = m.Read32(manager + 4);
  if (!Pointer(m, scene))
    return false;
  const u32 name = m.Read32(scene);
  for (size_t i = 0; i <= want.size(); ++i)
  {
    if (!m.Valid(name + static_cast<u32>(i)))
      return false;
    const char c = static_cast<char>(m.Read8(name + static_cast<u32>(i)));
    if (c != (i < want.size() ? want[i] : '\0'))
      return false;
  }
  return true;
}

std::u16string TagAt(const GuestMemory& m, u32 tag)
{
  std::u16string name;
  for (u32 i = 0; i < TAG_CHARS; ++i)
  {
    const u16 c = m.Read16(tag + 2 * i);
    if (c == 0)
      break;
    name.push_back(static_cast<char16_t>(c));
  }
  return name;
}

// The tag holding this name, else the highest unused one; -1 when every tag is taken.
int SlotFor(GuestMemory& m, u32 tags, const std::u16string& name)
{
  int unused = -1;
  for (int i = 0; i < TAG_SLOTS; ++i)
  {
    const u32 tag = tags + static_cast<u32>(i) * TAG_SIZE;
    const std::u16string held = TagAt(m, tag);
    if (held == name)
      return i;
    if (held.empty())
      unused = i;
  }
  if (unused < 0)
    return -1;
  const u32 tag = tags + static_cast<u32>(unused) * TAG_SIZE;
  for (u32 i = 0; i <= TAG_CHARS; ++i)
    m.Write16(tag + 2 * i, i < name.size() ? static_cast<u16>(name[i]) : 0);
  return unused;
}

class GuardMemory final : public GuestMemory
{
public:
  explicit GuardMemory(const Core::CPUThreadGuard& guard) : m_guard(guard) {}
  bool Valid(u32 address) const override
  {
    return PowerPC::MMU::HostIsRAMAddress(m_guard, address);
  }
  u32 Read32(u32 address) const override { return PowerPC::MMU::HostRead<u32>(m_guard, address); }
  u16 Read16(u32 address) const override { return PowerPC::MMU::HostRead<u16>(m_guard, address); }
  u8 Read8(u32 address) const override { return PowerPC::MMU::HostRead<u8>(m_guard, address); }
  void Write16(u32 address, u16 value) override
  {
    PowerPC::MMU::HostWrite<u16>(m_guard, value, address);
  }
  void Write32(u32 address, u32 value) override
  {
    PowerPC::MMU::HostWrite<u32>(m_guard, value, address);
  }

private:
  const Core::CPUThreadGuard& m_guard;
};

}  // namespace

std::u16string BrawlTag(std::string_view username)
{
  std::u16string tag;
  for (const char c : username)
  {
    if (tag.size() >= TAG_CHARS)
      break;
    if (c >= 'a' && c <= 'z')
      tag.push_back(static_cast<char16_t>(c - 'a' + 'A'));
    else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
             c == '!' || c == '?')
      tag.push_back(static_cast<char16_t>(c));
    else if (c == '_' || c == ' ')
      tag.push_back(u'-');
    // Anything else (other symbols, every byte of a non-ASCII character) is dropped.
  }
  return tag;
}

int ApplyNameTags(GuestMemory& m, const std::vector<Events::PortInfo>& ports)
{
  if (ports.empty() || !SceneIs(m, CHARACTER_SELECT))
    return 0;
  const u32 scene = m.Read32(m.Read32(SCENE_MANAGER) + 4);
  if (!Pointer(m, scene + SCENE_SELCHAR_TASK))
    return 0;
  const u32 task = m.Read32(scene + SCENE_SELCHAR_TASK);
  if (!Pointer(m, task + TASK_AREAS) || !Pointer(m, GAME_GLOBAL))
    return 0;
  const u32 global = m.Read32(GAME_GLOBAL);
  if (!Pointer(m, global + GAME_GLOBAL_NAME_RECORDS))
    return 0;
  const u32 records = m.Read32(global + GAME_GLOBAL_NAME_RECORDS);
  const u32 tags = records + RECORDS_TAGS;
  if (!Pointer(m, records) || !m.Valid(tags + TAG_SLOTS * TAG_SIZE - 1))
    return 0;
  int given = 0;
  for (const Events::PortInfo& p : ports)
  {
    if (p.port < 0 || p.port > 3)
      continue;
    const std::u16string name = BrawlTag(p.name);
    if (name.empty())
      continue;
    const u32 at = task + TASK_AREAS + 4 * static_cast<u32>(p.port);
    if (!Pointer(m, at))
      continue;
    const u32 area = m.Read32(at);
    if (!Pointer(m, area) || !Pointer(m, area + AREA_NAME_ID) ||
        m.Read32(area + AREA_INDEX) != static_cast<u32>(p.port) ||
        m.Read32(area + AREA_PLAYER_KIND) != PLAYER_KIND_HUMAN ||
        m.Read32(area + AREA_NAME_ID) != NO_TAG)
    {
      continue;
    }
    const int slot = SlotFor(m, tags, name);
    if (slot < 0)
      continue;
    m.Write32(area + AREA_NAME_ID, static_cast<u32>(slot));
    ++given;
  }
  return given;
}

void WriteNameTags(const Core::CPUThreadGuard& guard, const std::vector<Events::PortInfo>& ports)
{
  if (ports.empty())
    return;
  GuardMemory memory(guard);
  ApplyNameTags(memory, ports);
}

}  // namespace Orca::UX
