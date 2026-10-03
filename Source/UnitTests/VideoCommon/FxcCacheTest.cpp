// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Orca (ORCA.md, "Match start"): D3D's FXC cache keeps compiled shaders across Orca builds. What
// must hold: a binary only ever comes back for the very compile that made it (every input is in the
// key), and a damaged file never hands back bytes under any key (each record checks its own key and
// bytes; the read stops at the first that doesn't, and the file is written again from the ones
// before it).

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "VideoBackends/D3DCommon/FxcCache.h"

namespace
{
namespace FxcCache = D3DCommon::FxcCache;
using Digest = Common::SHA1::Digest;

constexpr std::string_view HLSL = "float4 main() : SV_Target { return float4(1, 0, 0, 1); }";
constexpr std::string_view ENTRY = "main";
constexpr std::string_view TARGET = "ps_5_0";
constexpr u32 FLAGS = 0x8002;  // D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_SKIP_VALIDATION
constexpr std::string_view MACROS = "API_D3D=1;";
// The file's header (magic, format) and each record's fixed part (size, key, check).
constexpr std::size_t HEADER = 8;
constexpr std::size_t RECORD = 4 + 20 + 20;

Digest Dll(u8 first)
{
  Digest digest{};
  digest[0] = first;
  return digest;
}

FxcCache::Key KeyOf(int n)
{
  return FxcCache::KeyFor(Dll(1), std::string(HLSL) + "// " + std::to_string(n), ENTRY, TARGET,
                          FLAGS, MACROS);
}

std::vector<u8> Bytes(std::size_t size, u8 seed)
{
  std::vector<u8> bytes(size);
  for (std::size_t i = 0; i < size; ++i)
    bytes[i] = static_cast<u8>(seed + i * 7);
  return bytes;
}

class FxcCacheTest : public testing::Test
{
protected:
  void SetUp() override
  {
    m_old_user = File::GetUserPath(D_USER_IDX);
    m_dir = File::CreateTempDir();
    ASSERT_FALSE(m_dir.empty());
    File::SetUserPath(D_USER_IDX, m_dir + DIR_SEP);
    ASSERT_TRUE(File::CreateFullPath(File::GetUserPath(D_SHADERCACHE_IDX)));
    FxcCache::Close();
  }

  void TearDown() override
  {
    FxcCache::Close();
    File::SetUserPath(D_USER_IDX, m_old_user);
    File::DeleteDirRecursively(m_dir);
  }

  static std::string CachePath() { return File::GetUserPath(D_SHADERCACHE_IDX) + "D3D-fxc.cache"; }

  static std::string ReadCache()
  {
    std::string bytes;
    File::ReadFileToString(CachePath(), bytes);
    return bytes;
  }

  static void WriteCache(const std::string& bytes)
  {
    ASSERT_TRUE(File::WriteStringToFile(CachePath(), bytes));
  }

  // A new run: what the last one wrote, read from the file.
  static void Reopen() { FxcCache::Close(); }

  std::string m_dir;
  std::string m_old_user;
};
}  // namespace

TEST(FxcCacheKey, EveryInputIsInTheKey)
{
  const FxcCache::Key key = FxcCache::KeyFor(Dll(1), HLSL, ENTRY, TARGET, FLAGS, MACROS);
  EXPECT_EQ(key, FxcCache::KeyFor(Dll(1), HLSL, ENTRY, TARGET, FLAGS, MACROS));

  const std::string other_hlsl = std::string(HLSL) + " ";
  const std::vector<FxcCache::Key> others = {
      FxcCache::KeyFor(Dll(2), HLSL, ENTRY, TARGET, FLAGS, MACROS),  // another d3dcompiler_47.dll
      FxcCache::KeyFor(Dll(1), other_hlsl, ENTRY, TARGET, FLAGS, MACROS),
      FxcCache::KeyFor(Dll(1), HLSL, "main2", TARGET, FLAGS, MACROS),
      FxcCache::KeyFor(Dll(1), HLSL, ENTRY, "vs_5_0", FLAGS, MACROS),
      FxcCache::KeyFor(Dll(1), HLSL, ENTRY, TARGET, FLAGS ^ 0x8000, MACROS),
      FxcCache::KeyFor(Dll(1), HLSL, ENTRY, TARGET, FLAGS, "API_D3D=2;"),
      FxcCache::KeyFor(Dll(1), HLSL, ENTRY, TARGET, FLAGS, ""),
  };
  for (std::size_t i = 0; i < others.size(); ++i)
  {
    EXPECT_NE(key, others[i]) << "input " << i;
    for (std::size_t j = 0; j < i; ++j)
      EXPECT_NE(others[i], others[j]) << "inputs " << i << " and " << j;
  }
  // The same bytes split between two inputs another way.
  EXPECT_NE(FxcCache::KeyFor(Dll(1), "ab", "c", TARGET, FLAGS, MACROS),
            FxcCache::KeyFor(Dll(1), "a", "bc", TARGET, FLAGS, MACROS));
}

TEST_F(FxcCacheTest, KeptAcrossRuns)
{
  EXPECT_FALSE(FxcCache::Find(KeyOf(1)));
  FxcCache::Store(KeyOf(1), Bytes(100, 1));
  FxcCache::Store(KeyOf(2), Bytes(3000, 2));
  // In this run already.
  EXPECT_EQ(FxcCache::Find(KeyOf(2)), Bytes(3000, 2));
  // The same compile again adds nothing.
  FxcCache::Store(KeyOf(2), Bytes(3000, 2));
  EXPECT_EQ(ReadCache().size(), HEADER + 2 * RECORD + 100 + 3000);

  Reopen();
  EXPECT_EQ(FxcCache::Find(KeyOf(1)), Bytes(100, 1));
  EXPECT_EQ(FxcCache::Find(KeyOf(2)), Bytes(3000, 2));
  EXPECT_FALSE(FxcCache::Find(KeyOf(3)));
  // A compile that differs in any input is another key (EveryInputIsInTheKey): a miss, compiled.
  EXPECT_FALSE(FxcCache::Find(
      FxcCache::KeyFor(Dll(2), std::string(HLSL) + "// 1", ENTRY, TARGET, FLAGS, MACROS)));
}

TEST_F(FxcCacheTest, TornRecordKeepsTheOnesBefore)
{
  FxcCache::Store(KeyOf(1), Bytes(100, 1));
  FxcCache::Store(KeyOf(2), Bytes(200, 2));
  FxcCache::Store(KeyOf(3), Bytes(300, 3));
  Reopen();
  const std::string whole = ReadCache();
  ASSERT_EQ(whole.size(), HEADER + 3 * RECORD + 600);

  // A process killed in the middle of its last record.
  WriteCache(whole.substr(0, whole.size() - 7));
  EXPECT_EQ(FxcCache::Find(KeyOf(1)), Bytes(100, 1));
  EXPECT_EQ(FxcCache::Find(KeyOf(2)), Bytes(200, 2));
  EXPECT_FALSE(FxcCache::Find(KeyOf(3)));
  // Written again from the two whole records, and appended to from there.
  EXPECT_EQ(ReadCache().size(), HEADER + 2 * RECORD + 300);
  FxcCache::Store(KeyOf(3), Bytes(300, 3));
  Reopen();
  EXPECT_EQ(FxcCache::Find(KeyOf(3)), Bytes(300, 3));
  // The same three records (rewritten in key order).
  EXPECT_EQ(ReadCache().size(), whole.size());
}

TEST_F(FxcCacheTest, AlteredBytesAreNeverReturned)
{
  FxcCache::Store(KeyOf(1), Bytes(100, 1));
  FxcCache::Store(KeyOf(2), Bytes(200, 2));
  FxcCache::Store(KeyOf(3), Bytes(300, 3));
  Reopen();
  const std::string whole = ReadCache();

  // One byte of the second record's bytecode, then one of its key: neither record is used, nor
  // anything after it, under any key. Two processes appending at once read the same way: the first
  // record whose check fails ends the read.
  for (const std::size_t at : {HEADER + RECORD + 100 + RECORD + 50, HEADER + RECORD + 100 + 4 + 3})
  {
    std::string damaged = whole;
    damaged[at] = static_cast<char>(damaged[at] ^ 0x10);
    WriteCache(damaged);
    Reopen();
    EXPECT_EQ(FxcCache::Find(KeyOf(1)), Bytes(100, 1));
    EXPECT_FALSE(FxcCache::Find(KeyOf(2)));
    EXPECT_FALSE(FxcCache::Find(KeyOf(3)));
    EXPECT_EQ(ReadCache().size(), HEADER + RECORD + 100);
  }
}

TEST_F(FxcCacheTest, ForeignFileStartsOver)
{
  for (const std::string& foreign :
       {std::string("not a cache"), std::string("OFXC\x02\0\0\0", 8), std::string()})
  {
    WriteCache(foreign);
    Reopen();
    EXPECT_FALSE(FxcCache::Find(KeyOf(1)));
    FxcCache::Store(KeyOf(1), Bytes(64, 9));
    Reopen();
    EXPECT_EQ(FxcCache::Find(KeyOf(1)), Bytes(64, 9));
    EXPECT_EQ(ReadCache().size(), HEADER + RECORD + 64);
  }
}
