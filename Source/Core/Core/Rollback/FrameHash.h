// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// A hash of all guest RAM (MEM1 and MEM2) every frame, at the cost of the memory written since the
// previous frame.
//
// RAM is cut into 4 KB blocks, each hashed with its physical address as the seed, and the frame's
// hash is the sum of the block hashes. It depends only on RAM's bytes, so every machine computes
// the same value whatever its host page size and whichever pages its copy-on-write tracking
// happened to open. Each Update hashes again the blocks written since the previous one (which
// copy-on-write tracking already knows, Cow::TakeTouched) plus a rotating 1/AUDIT_FRAMES of all
// blocks. The audit never changes the value while tracking is complete; a stale block it finds is
// a write the tracking missed, counted in AuditMisses and correct from then on.

#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

namespace Rollback
{
class FrameHash
{
public:
  static constexpr u32 BLOCK = 4096;
  // At 60 frames a second the audit reads all of RAM every 6 seconds.
  static constexpr u32 AUDIT_FRAMES = 360;

  // One area of RAM, readable at `data`. Sizes are multiples of BLOCK.
  struct Area
  {
    const u8* data;
    u32 physical_address;
    u32 size;
  };

  // Hashes everything again at the next Update.
  void Reset() { m_valid = false; }
  // Brings the hash up to date and returns it. `touched` lists (physical address, length) ranges
  // that may have changed since the previous Update; null means unknown, and everything is hashed.
  // A layout other than the previous one's also hashes everything.
  u64 Update(const std::vector<Area>& areas, const std::vector<std::pair<u32, u32>>* touched);
  u64 Value() const { return m_sum; }
  // Blocks the audit found changed although no range named them.
  u64 AuditMisses() const { return m_audit_misses; }
  // Blocks the last Update read, audit included.
  std::size_t LastBlocks() const { return m_last_blocks; }

  // The same hash of `areas`, from scratch.
  static u64 Full(const std::vector<Area>& areas);

private:
  static u64 HashBlock(const Area& area, u32 offset);
  void Rebuild(const std::vector<Area>& areas);
  void Rehash(std::size_t area, u32 offset);

  std::vector<Area> m_areas;
  std::vector<std::size_t> m_first_block;
  std::vector<u64> m_blocks;
  u64 m_sum = 0;
  bool m_valid = false;
  std::size_t m_audit_next = 0;
  u64 m_audit_misses = 0;
  std::size_t m_last_blocks = 0;
};
}  // namespace Rollback
