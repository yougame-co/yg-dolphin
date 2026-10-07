// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "Common/ChunkFile.h"

namespace
{
template <typename T>
auto Bytes(T value)
{
  std::array<u8, sizeof(T)> bytes;
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}
}  // namespace

TEST(ChunkFile, RejectsTruncatedScalarWithoutChangingDestination)
{
  std::array<u8, 3> bytes{};
  u8* cursor = bytes.data();
  PointerWrap p(&cursor, bytes.size(), PointerWrap::Mode::Read);
  u32 value = 123;
  EXPECT_THROW(p.Do(value), PointerWrap::ReadError);
  EXPECT_EQ(value, 123u);
  EXPECT_EQ(cursor, bytes.data());
}

TEST(ChunkFile, RejectsContainerCountBeforeAllocating)
{
  auto bytes = Bytes(std::numeric_limits<u32>::max());
  u8* cursor = bytes.data();
  PointerWrap p(&cursor, bytes.size(), PointerWrap::Mode::Read);
  std::vector<u64> values{1, 2};
  EXPECT_THROW(p.Do(values), PointerWrap::ReadError);
  EXPECT_EQ(values, (std::vector<u64>{1, 2}));
}

TEST(ChunkFile, RejectsMultiByteContainerWithoutEnoughPayload)
{
  std::array<u32, 3> bytes{3, 42, 43};
  u8* cursor = reinterpret_cast<u8*>(bytes.data());
  PointerWrap p(&cursor, sizeof(bytes), PointerWrap::Mode::Read);
  std::vector<u32> values{123};
  EXPECT_THROW(p.Do(values), PointerWrap::ReadError);
  EXPECT_EQ(values, (std::vector<u32>{123}));
}

TEST(ChunkFile, RejectsComplexContainerExpansion)
{
  std::array<u8, 8> bytes{};
  const u32 count = 1;
  std::memcpy(bytes.data(), &count, sizeof(count));
  u8* cursor = bytes.data();
  PointerWrap p(&cursor, bytes.size(), PointerWrap::Mode::Read, 64 * 1024 * 1024);
  std::vector<std::array<u8, 65 * 1024 * 1024>> values;
  EXPECT_THROW(p.DoEachElement(values, [](PointerWrap&, auto&) {}), PointerWrap::ReadError);
  EXPECT_TRUE(values.empty());
}

TEST(ChunkFile, RejectsMapAndSetCountsBeforeClearing)
{
  auto bytes = Bytes(std::numeric_limits<u32>::max());
  for (bool map : {true, false})
  {
    u8* cursor = bytes.data();
    PointerWrap p(&cursor, bytes.size(), PointerWrap::Mode::Read);
    if (map)
    {
      std::map<u32, u32> values{{1, 2}};
      EXPECT_THROW(p.Do(values), PointerWrap::ReadError);
      EXPECT_EQ(values.size(), 1u);
    }
    else
    {
      std::set<u32> values{1};
      EXPECT_THROW(p.Do(values), PointerWrap::ReadError);
      EXPECT_EQ(values.size(), 1u);
    }
  }
}

TEST(ChunkFile, RejectsInvalidPointerOffsetsWithoutChangingPointer)
{
  std::array<u8, 32> destination{};
  for (const ptrdiff_t offset : std::array<ptrdiff_t, 4>{-1, 33,
                                                       std::numeric_limits<ptrdiff_t>::max(),
                                                       std::numeric_limits<ptrdiff_t>::min()})
  {
    auto bytes = Bytes(offset);
    u8* cursor = bytes.data();
    PointerWrap p(&cursor, bytes.size(), PointerWrap::Mode::Read);
    u8* pointer = destination.data() + 5;
    EXPECT_THROW(p.DoPointer(pointer, destination.data(), destination.size()), PointerWrap::ReadError);
    EXPECT_EQ(pointer, destination.data() + 5);
  }
}

TEST(ChunkFile, AllowsPointerAtEndOfBuffer)
{
  std::array<u8, 32> destination{};
  auto bytes = Bytes(ptrdiff_t{32});
  u8* cursor = bytes.data();
  PointerWrap p(&cursor, bytes.size(), PointerWrap::Mode::Read);
  u8* pointer = destination.data();
  p.DoPointer(pointer, destination.data(), destination.size());
  EXPECT_EQ(pointer, destination.data() + destination.size());
}

TEST(ChunkFile, RejectsExternalDataOverrunBeforeReturningPointer)
{
  auto bytes = Bytes(std::numeric_limits<u32>::max());
  u8* cursor = bytes.data();
  PointerWrap p(&cursor, bytes.size(), PointerWrap::Mode::Read);
  u32 size = 0;
  EXPECT_THROW((void)p.DoExternal(size), PointerWrap::ReadError);
  EXPECT_EQ(cursor, bytes.data() + bytes.size());
}

TEST(ChunkFile, ReadErrorsUnwindInsteadOfMeasuring)
{
  auto bytes = Bytes(u32{0});
  u8* cursor = bytes.data();
  PointerWrap p(&cursor, bytes.size(), PointerWrap::Mode::Read);
  EXPECT_THROW(p.DoMarker("test"), PointerWrap::ReadError);
  EXPECT_THROW(p.SetMeasureMode(), PointerWrap::ReadError);
}

TEST(ChunkFile, ValidContainersAndPointersRoundTrip)
{
  std::array<u8, 1024> bytes{};
  std::array<u8, 32> destination{};
  u8* cursor = bytes.data();
  PointerWrap writer(&cursor, bytes.size(), PointerWrap::Mode::Write);
  std::vector<u64> values{1, 2, 3};
  std::string text = "valid state";
  u8* pointer = destination.data() + 9;
  writer.Do(values);
  writer.Do(text);
  writer.DoPointer(pointer, destination.data(), destination.size());
  const size_t size = cursor - bytes.data();
  cursor = bytes.data();
  PointerWrap reader(&cursor, size, PointerWrap::Mode::Read);
  values.clear();
  text.clear();
  pointer = destination.data();
  reader.Do(values);
  reader.Do(text);
  reader.DoPointer(pointer, destination.data(), destination.size());
  EXPECT_EQ(values, (std::vector<u64>{1, 2, 3}));
  EXPECT_EQ(text, "valid state");
  EXPECT_EQ(pointer, destination.data() + 9);
  EXPECT_EQ(cursor, bytes.data() + size);
}
