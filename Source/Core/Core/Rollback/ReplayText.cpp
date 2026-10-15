// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/ReplayText.h"

#include <xxh3.h>

#include "Core/Orca/Engine/GuestMemory.h"

namespace Rollback
{
void ReplayText::Write8(Orca::UX::GuestMemory& memory, u32 address, u8 value)
{
  const u8 current = memory.Read8(address);
  if (s_current && current != value)
  {
    auto& bytes = s_current->m_bytes;
    if (bytes.use_count() != 1)
      bytes = std::make_shared<std::map<u32, Byte>>(*bytes);
    const u32 physical = address & 0x3fffffff;
    const auto previous = bytes->find(physical);
    const u8 before = previous != bytes->end() && previous->second.text == current ?
                          previous->second.before : current;
    (*bytes)[physical] = {before, value};
  }
  memory.Write8(address, value);
}

void ReplayText::Write16(Orca::UX::GuestMemory& memory, u32 address, u16 value)
{
  Write8(memory, address, static_cast<u8>(value >> 8));
  Write8(memory, address + 1, static_cast<u8>(value));
}

void ReplayText::Write32(Orca::UX::GuestMemory& memory, u32 address, u32 value)
{
  Write16(memory, address, static_cast<u16>(value >> 16));
  Write16(memory, address + 2, static_cast<u16>(value));
}

u8 ReplayText::VerifiedByte(u32 address, u8 value) const
{
  const auto it = m_bytes->find(address & 0x3fffffff);
  return it != m_bytes->end() && it->second.text == value ? it->second.before : value;
}

u64 ReplayText::Checksum(std::span<const u8> mem1, std::span<const u8> mem2) const
{
  XXH3_state_t* const state = XXH3_createState();
  XXH3_64bits_reset(state);
  const auto update = [&](std::span<const u8> ram, u32 base) {
    size_t offset = 0;
    for (auto it = m_bytes->lower_bound(base);
         it != m_bytes->end() && it->first - base < ram.size(); ++it)
    {
      const size_t at = it->first - base;
      XXH3_64bits_update(state, ram.data() + offset, at - offset);
      const u8 byte = VerifiedByte(it->first, ram[at]);
      XXH3_64bits_update(state, &byte, 1);
      offset = at + 1;
    }
    if (offset < ram.size())
      XXH3_64bits_update(state, ram.data() + offset, ram.size() - offset);
  };
  update(mem1, 0);
  update(mem2, 0x10000000);
  const u64 hash = XXH3_64bits_digest(state);
  XXH3_freeState(state);
  return hash;
}
}  // namespace Rollback
