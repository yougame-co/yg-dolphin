// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Core/Rollback/FrameHash.h"

using Rollback::FrameHash;

namespace
{
constexpr u32 MEM2 = 0x10000000;

// RAM shaped like MEM1 and MEM2, filled with noise.
struct Ram
{
  Ram(u32 mem1, u32 mem2) : mem1(mem1), mem2(mem2)
  {
    std::mt19937 rng(7);
    for (u8& b : this->mem1)
      b = static_cast<u8>(rng());
    for (u8& b : this->mem2)
      b = static_cast<u8>(rng());
  }
  std::vector<FrameHash::Area> Areas() const
  {
    return {{mem1.data(), 0, static_cast<u32>(mem1.size())},
            {mem2.data(), MEM2, static_cast<u32>(mem2.size())}};
  }
  std::vector<u8> mem1;
  std::vector<u8> mem2;
};

using Ranges = std::vector<std::pair<u32, u32>>;

TEST(OrcaFrameHash, IncrementalMatchesFromScratch)
{
  Ram ram(64 * FrameHash::BLOCK, 96 * FrameHash::BLOCK);
  FrameHash hash;
  EXPECT_EQ(hash.Update(ram.Areas(), nullptr), FrameHash::Full(ram.Areas()));
  std::mt19937 rng(3);
  for (int frame = 0; frame < 500; ++frame)
  {
    Ranges touched;
    const int writes = static_cast<int>(rng() % 12);
    for (int w = 0; w < writes; ++w)
    {
      const bool second = rng() % 2;
      std::vector<u8>& area = second ? ram.mem2 : ram.mem1;
      const u32 offset = static_cast<u32>(rng() % area.size());
      const u32 length = std::min<u32>(1 + rng() % 9000, static_cast<u32>(area.size()) - offset);
      for (u32 i = 0; i < length; ++i)
        area[offset + i] ^= static_cast<u8>(1 + rng() % 255);
      // Ranges as copy-on-write reports them (whole host pages) or exactly the bytes written.
      const u32 base = second ? MEM2 : 0;
      if (rng() % 2)
        touched.emplace_back(base + offset, length);
      else
        touched.emplace_back(base + offset / 16384 * 16384,
                             std::min<u32>((offset + length + 16383) / 16384 * 16384,
                                           static_cast<u32>(area.size())) -
                                 offset / 16384 * 16384);
    }
    ASSERT_EQ(hash.Update(ram.Areas(), &touched), FrameHash::Full(ram.Areas())) << frame;
  }
  EXPECT_EQ(hash.AuditMisses(), 0u);
}

// The hash depends on the bytes and where they are, not on how RAM is split into areas or which
// machine computed it.
TEST(OrcaFrameHash, DependsOnBytesAndAddressesOnly)
{
  Ram ram(8 * FrameHash::BLOCK, 8 * FrameHash::BLOCK);
  const u64 whole = FrameHash::Full(ram.Areas());
  std::vector<FrameHash::Area> split = {{ram.mem1.data(), 0, 3 * FrameHash::BLOCK},
                                        {ram.mem1.data() + 3 * FrameHash::BLOCK,
                                         3 * FrameHash::BLOCK, 5 * FrameHash::BLOCK},
                                        ram.Areas()[1]};
  EXPECT_EQ(FrameHash::Full(split), whole);
  // The same bytes elsewhere are another state.
  std::vector<FrameHash::Area> moved = ram.Areas();
  moved[1].physical_address += FrameHash::BLOCK;
  EXPECT_NE(FrameHash::Full(moved), whole);
  // Two blocks swapped are another state too, although the sum of block hashes is order-free.
  std::swap_ranges(ram.mem1.begin(), ram.mem1.begin() + FrameHash::BLOCK,
                   ram.mem1.begin() + FrameHash::BLOCK);
  EXPECT_NE(FrameHash::Full(ram.Areas()), whole);
}

// A single changed byte anywhere changes the very next value; an identical copy agrees.
TEST(OrcaFrameHash, OneByteShowsAtTheNextUpdate)
{
  Ram here(32 * FrameHash::BLOCK, 32 * FrameHash::BLOCK);
  Ram there = here;
  FrameHash a, b;
  ASSERT_EQ(a.Update(here.Areas(), nullptr), b.Update(there.Areas(), nullptr));
  std::mt19937 rng(11);
  for (int trial = 0; trial < 200; ++trial)
  {
    const bool second = rng() % 2;
    const u32 offset = static_cast<u32>(rng() % here.mem1.size());
    const u8 bit = static_cast<u8>(1u << (rng() % 8));
    const u32 address = (second ? MEM2 : 0) + offset;
    const Ranges touched = {{address, 1}};
    (second ? there.mem2 : there.mem1)[offset] ^= bit;
    EXPECT_NE(a.Update(here.Areas(), &touched), b.Update(there.Areas(), &touched)) << trial;
    (second ? there.mem2 : there.mem1)[offset] ^= bit;
    EXPECT_EQ(a.Update(here.Areas(), &touched), b.Update(there.Areas(), &touched)) << trial;
  }
}

// A write nobody reported is found by the audit within AUDIT_FRAMES updates and counted.
TEST(OrcaFrameHash, AuditFindsAnUnreportedWrite)
{
  Ram ram(40 * FrameHash::BLOCK, 80 * FrameHash::BLOCK);
  FrameHash hash;
  hash.Update(ram.Areas(), nullptr);
  ram.mem2[77 * FrameHash::BLOCK + 5] ^= 0x10;
  const Ranges none;
  int found = -1;
  for (u32 frame = 0; frame < FrameHash::AUDIT_FRAMES && found < 0; ++frame)
  {
    hash.Update(ram.Areas(), &none);
    if (hash.AuditMisses() > 0)
      found = static_cast<int>(frame);
  }
  EXPECT_GE(found, 0);
  EXPECT_EQ(hash.AuditMisses(), 1u);
  EXPECT_EQ(hash.Value(), FrameHash::Full(ram.Areas()));
  // A new layout hashes everything again.
  std::vector<FrameHash::Area> shorter = ram.Areas();
  shorter[1].size -= FrameHash::BLOCK;
  EXPECT_EQ(hash.Update(shorter, &none), FrameHash::Full(shorter));
}

// What a frame costs on Wii-sized RAM (24 MB + 64 MB) by how much of it the frame wrote, the
// audit included, and what the first, full hash costs.
TEST(OrcaFrameHash, CostPerFrame)
{
  Ram ram(24 << 20, 64 << 20);
  FrameHash hash;
  auto start = std::chrono::steady_clock::now();
  hash.Update(ram.Areas(), nullptr);
  const double full_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::printf("[ FRAMEHASH ] full hash of 88 MB: %.2f ms\n", full_ms);
  std::mt19937 rng(5);
  for (const u32 kb : {0u, 256u, 1024u, 4096u, 16384u})
  {
    constexpr int FRAMES = 120;
    double total_ms = 0;
    for (int frame = 0; frame < FRAMES; ++frame)
    {
      // Scattered 16 KB pages, as copy-on-write reports them.
      Ranges touched;
      for (u32 page = 0; page < kb / 16; ++page)
      {
        const bool second = rng() % 3 != 0;
        const u32 size = static_cast<u32>((second ? ram.mem2 : ram.mem1).size());
        const u32 offset = static_cast<u32>(rng() % (size / 16384)) * 16384;
        (second ? ram.mem2 : ram.mem1)[offset + rng() % 16384] ^= 1;
        touched.emplace_back((second ? MEM2 : 0) + offset, 16384);
      }
      std::sort(touched.begin(), touched.end());
      start = std::chrono::steady_clock::now();
      hash.Update(ram.Areas(), &touched);
      total_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                            start)
                      .count();
    }
    std::printf("[ FRAMEHASH ] %5u KB written a frame: %.3f ms a frame (%zu KB read)\n", kb,
                total_ms / FRAMES, hash.LastBlocks() * FrameHash::BLOCK / 1024);
  }
  EXPECT_EQ(hash.Value(), FrameHash::Full(ram.Areas()));
  EXPECT_EQ(hash.AuditMisses(), 0u);
}
}  // namespace
