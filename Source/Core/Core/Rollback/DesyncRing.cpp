// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/DesyncRing.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>

#include <xxh3.h>

namespace Rollback::DesyncRing
{
namespace
{
constexpr std::size_t RING_SIZE = 4096;
std::mutex s_lock;
std::array<Entry, RING_SIZE> s_ring;
// Entries recorded since the last reset; the newest is at (s_count - 1) % RING_SIZE.
u64 s_count = 0;
std::atomic<int> s_frame{-1};

void Push(Kind kind, u32 address, u32 length, u32 hash)
{
  const int frame = s_frame.load(std::memory_order_relaxed);
  std::lock_guard lock(s_lock);
  s_ring[s_count % RING_SIZE] = Entry{frame, kind, address, length, hash};
  ++s_count;
}
}  // namespace

bool On()
{
  static const bool s_on = [] {
    const char* v = std::getenv("YG_DESYNC_MAP");
    return v && v[0] == '1';
  }();
  return s_on;
}

void SetFrame(int frame)
{
  if (!On())
    return;
  s_frame.store(frame, std::memory_order_relaxed);
}

int Frame()
{
  return s_frame.load(std::memory_order_relaxed);
}

void Record(Kind kind, u32 address, const u8* data, u32 length)
{
  if (!On())
    return;
  const u32 hash = data && length ? static_cast<u32>(XXH3_64bits(data, length) >> 32) : 0;
  Push(kind, address, length, hash);
}

void RecordRows(Kind kind, u32 address, const u8* data, u32 row_bytes, u32 rows, u32 stride)
{
  if (!On())
    return;
  // Only the bytes the rows write, each row's hash seeding the next.
  u64 hash = 0;
  if (data)
  {
    for (u32 row = 0; row < rows; ++row)
      hash = XXH3_64bits_withSeed(data + std::size_t{row} * stride, row_bytes, hash);
  }
  const u64 span = rows ? u64{rows - 1} * stride + row_bytes : 0;
  Push(kind, address, static_cast<u32>(std::min<u64>(span, 0xffffffffu)),
       data ? static_cast<u32>(hash >> 32) : 0);
}

std::vector<Entry> Entries(u64* recorded)
{
  std::lock_guard lock(s_lock);
  const std::size_t held = static_cast<std::size_t>(std::min<u64>(s_count, RING_SIZE));
  std::vector<Entry> out;
  out.reserve(held);
  for (u64 i = s_count - held; i < s_count; ++i)
    out.push_back(s_ring[i % RING_SIZE]);
  if (recorded)
    *recorded = s_count;
  return out;
}

void Reset()
{
  if (!On())
    return;
  std::lock_guard lock(s_lock);
  s_count = 0;
  s_frame.store(-1, std::memory_order_relaxed);
}

const char* KindName(Kind kind)
{
  switch (kind)
  {
  case Kind::Efb:
    return "efb";
  case Kind::EfbZero:
    return "efbz";
  case Kind::EfbSkip:
    return "efbskip";
  case Kind::Dvd:
    return "dvd";
  case Kind::IosFs:
    return "iosfs";
  case Kind::Sdio:
    return "sdio";
  }
  return "?";
}
}  // namespace Rollback::DesyncRing
