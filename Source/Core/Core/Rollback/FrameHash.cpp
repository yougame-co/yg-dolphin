// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/FrameHash.h"

#include <algorithm>

#include <xxh3.h>

namespace Rollback
{
u64 FrameHash::HashBlock(const Area& area, u32 offset)
{
  return XXH3_64bits_withSeed(area.data + offset, BLOCK, u64{area.physical_address} + offset);
}

u64 FrameHash::Full(const std::vector<Area>& areas)
{
  u64 sum = 0;
  for (const Area& area : areas)
  {
    for (u32 offset = 0; offset + BLOCK <= area.size; offset += BLOCK)
      sum += HashBlock(area, offset);
  }
  return sum;
}

void FrameHash::Rebuild(const std::vector<Area>& areas)
{
  m_areas = areas;
  m_first_block.clear();
  std::size_t blocks = 0;
  for (const Area& area : m_areas)
  {
    m_first_block.push_back(blocks);
    blocks += area.size / BLOCK;
  }
  m_blocks.assign(blocks, 0);
  m_sum = 0;
  for (std::size_t a = 0; a < m_areas.size(); ++a)
  {
    for (u32 offset = 0; offset + BLOCK <= m_areas[a].size; offset += BLOCK)
    {
      const u64 hash = HashBlock(m_areas[a], offset);
      m_blocks[m_first_block[a] + offset / BLOCK] = hash;
      m_sum += hash;
    }
  }
  m_audit_next = 0;
  m_last_blocks = blocks;
  m_valid = true;
}

void FrameHash::Rehash(std::size_t area, u32 offset)
{
  u64& cached = m_blocks[m_first_block[area] + offset / BLOCK];
  const u64 hash = HashBlock(m_areas[area], offset);
  m_sum += hash - cached;
  cached = hash;
  ++m_last_blocks;
}

u64 FrameHash::Update(const std::vector<Area>& areas,
                      const std::vector<std::pair<u32, u32>>* touched)
{
  const auto same = [](const Area& a, const Area& b) {
    return a.data == b.data && a.physical_address == b.physical_address && a.size == b.size;
  };
  if (!m_valid || !touched ||
      !std::equal(areas.begin(), areas.end(), m_areas.begin(), m_areas.end(), same))
  {
    Rebuild(areas);
    return m_sum;
  }
  m_last_blocks = 0;
  for (const auto& [address, length] : *touched)
  {
    for (std::size_t a = 0; a < m_areas.size(); ++a)
    {
      const Area& area = m_areas[a];
      const u64 start = std::max<u64>(address, area.physical_address);
      const u64 end = std::min<u64>(u64{address} + length, u64{area.physical_address} + area.size);
      if (start >= end)
        continue;
      const u32 first = static_cast<u32>((start - area.physical_address) / BLOCK * BLOCK);
      for (u32 offset = first; area.physical_address + u64{offset} < end; offset += BLOCK)
        Rehash(a, offset);
    }
  }
  // The audit: a rotating slice of every block, read again from RAM.
  const std::size_t total = m_blocks.size();
  if (total > 0)
  {
    const std::size_t slice = (total + AUDIT_FRAMES - 1) / AUDIT_FRAMES;
    for (std::size_t i = 0; i < slice; ++i)
    {
      const std::size_t block = m_audit_next;
      m_audit_next = (m_audit_next + 1) % total;
      const std::size_t area = static_cast<std::size_t>(
          std::upper_bound(m_first_block.begin(), m_first_block.end(), block) -
          m_first_block.begin() - 1);
      const u32 offset = static_cast<u32>((block - m_first_block[area]) * BLOCK);
      const u64 hash = HashBlock(m_areas[area], offset);
      ++m_last_blocks;
      if (hash == m_blocks[block])
        continue;
      ++m_audit_misses;
      m_sum += hash - m_blocks[block];
      m_blocks[block] = hash;
    }
  }
  return m_sum;
}
}  // namespace Rollback
