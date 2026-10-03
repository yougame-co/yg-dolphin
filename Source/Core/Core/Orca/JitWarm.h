// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

struct JitBlock;
namespace Core
{
class CPUThreadGuard;
}

// JIT warm-up (ORCA.md, "Match start"). The first time a match runs, Dolphin's JIT compiles the
// match's code at its first use: ~16,500 blocks in the two frames where the stage and the first
// fighter appear (64-105 ms each on an M4), ~4,100 more when the second fighter enters during the
// countdown. Orca remembers which blocks a match compiled (start address, the physical ranges of
// their instructions, and a hash of those words as they were in RAM) and, from the next match on,
// compiles the ones whose words are in RAM again while the match loads, a few ms per frame, before
// the game first runs them.
//
// Host only. A block compiled ahead of its first run is the block the JIT would compile then: the
// same words at the same addresses, the same feature flags, compiled in the guest's floating-point
// mode as the dispatcher does, and kept only if its instructions span exactly the recorded ranges.
// Like the JIT's own blocks after a rollback (Rollback.cpp keeps them across loads), it relies on
// the game invalidating code it rewrites, so a block whose words changed within a second of its
// compile in the recorded match is never remembered. Only instruction-BAT-mapped code is touched (no
// page-table walk, so no TLB or page-table change), and nothing is compiled when the compile could
// clear or reuse code memory under the running block (JitBase::CanCompileFromHook).
namespace Orca::JitWarm
{
struct Block
{
  u32 effective = 0;  // the block's start, where the game branches to
  u32 physical = 0;   // where an instruction BAT maps it
  u32 flags = 0;      // the JIT's feature flags (MSR.DR, MSR.IR, HID2.PSE) it was compiled with
  u64 hash = 0;       // the ranges and the instruction words in them, as in RAM at its compile
  u16 when = 0;       // first-pass frames from the match's start to its first compile (capped)
  u8 age = 0;         // matches since a match last compiled it or found it compiled
  std::vector<std::pair<u32, u32>> ranges;  // physical [start, end) of its instructions
};

// What a block's hash would be with RAM as it is now; nullopt when a range isn't in MEM1 or MEM2.
std::optional<u64> HashRanges(std::span<const u8> mem1, std::span<const u8> mem2,
                              const std::vector<std::pair<u32, u32>>& ranges);

// The file (<user>/Cache/<profile>.jitwarm): a header, the blocks, and a checksum.
std::vector<u8> Encode(const std::vector<Block>& blocks);
std::optional<std::vector<Block>> Decode(std::span<const u8> bytes);

// After a match: blocks in `known` found compiled during it (`seen`, by index) are young again, the
// rest age a match; `learned` (the blocks the match compiled itself) join or refresh theirs; blocks
// older than MAX_AGE matches go, and past MAX_BLOCKS the oldest go.
constexpr int MAX_AGE = 24;
constexpr std::size_t MAX_BLOCKS = 200'000;
std::vector<Block> Merge(std::vector<Block> known, const std::vector<bool>& seen,
                         std::vector<Block> learned);

// The first sweep's pace (JitWarm.cpp, LEAD): `due` lists, in queue order, each first-run frame
// (`when`) and the queue index past its last block; the sweep is at `cursor` at the boundary before
// frame `frame`. Returns how far it must get at this boundary so that each block is passed by the
// boundary `lead` frames before its `when`, every deadline's remaining blocks spread evenly over
// the boundaries left to it (all of them now once it has passed).
std::size_t PacedTarget(std::span<const std::pair<int, std::size_t>> due, std::size_t cursor,
                        int frame, int lead);

// Every block the JIT finishes (JitBaseBlockCache::FinalizeBlock). Recorded during a match's first
// RECORD_FRAMES frames, never while warming.
void OnBlockFinalized(const JitBlock& block);

// The session's frame boundary (Rollback::OnFrameBoundary), host floating-point mode, at every
// boundary whose next frame is a first pass.
void OnBoundary(const Core::CPUThreadGuard& guard);
}  // namespace Orca::JitWarm
