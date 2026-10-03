// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Orca (ORCA.md, "Match start"): D3D's FXC cache keeps compiled shaders across Orca builds. What
// must hold: a binary only ever comes back for the very compile that made it (every input is in the
// key), and a damaged file never hands back bytes under any key (each record checks its own key and
// bytes; the read stops at the first that doesn't, and the file is written again from the ones
// before it). The file stays open while a run lasts, and another Orca on the same user folder may
// read it and append to it meanwhile (SharedWithAnotherOrca). On Windows a plain open of a file
// that something else has open for writing fails, so the tests end the run (Reopen) before reading
// the file the plain way or damaging it, and read it mid-run the way another Orca does.

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
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

  // The file as any program reads it; on Windows only once no run has it open, which this checks
  // too: Close lets it go.
  static std::string ReadCache()
  {
    std::string bytes;
    EXPECT_TRUE(File::ReadFileToString(CachePath(), bytes)) << "is the cache still open?";
    return bytes;
  }

  // The file read while a run has it open, the way another Orca reads it: sharing it with the
  // run's writes.
  static std::string ReadCacheWhileOpen()
  {
    File::IOFile in(CachePath(), "rb", File::SharedAccess::ReadWrite);
    std::string bytes(in.GetSize(), '\0');
    EXPECT_TRUE(in.ReadBytes(bytes.data(), bytes.size())) << "the cache can't be read while open";
    return bytes;
  }

  // Between runs: on Windows nothing else can write it while a run has it open.
  static void WriteCache(const std::string& bytes)
  {
    ASSERT_TRUE(File::WriteStringToFile(CachePath(), bytes)) << "is the cache still open?";
  }

  // A new run: what the last one wrote, read from the file.
  static void Reopen() { FxcCache::Close(); }

  // One record as the cache writes it (FxcCache.cpp, "The file"): another Orca's.
  static std::string Record(const FxcCache::Key& key, const std::vector<u8>& bytes)
  {
    auto context = Common::SHA1::CreateContext();
    context->Update(key.data(), key.size());
    context->Update(bytes.data(), bytes.size());
    const Digest check = context->Finish();
    const u32 size = static_cast<u32>(bytes.size());
    std::string record(reinterpret_cast<const char*>(&size), sizeof(size));
    record.append(reinterpret_cast<const char*>(key.data()), key.size());
    record.append(reinterpret_cast<const char*>(check.data()), check.size());
    record.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return record;
  }

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
  // On disk already, while the run goes on (a killed Orca keeps them), and readable meanwhile.
  EXPECT_EQ(ReadCacheWhileOpen().size(), HEADER + 2 * RECORD + 100 + 3000);

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
  // Written again from the two whole records, and appended to from there in the same run.
  EXPECT_EQ(ReadCacheWhileOpen().size(), HEADER + 2 * RECORD + 300);
  FxcCache::Store(KeyOf(3), Bytes(300, 3));
  Reopen();
  EXPECT_EQ(FxcCache::Find(KeyOf(3)), Bytes(300, 3));
  // The same three records (rewritten in key order).
  Reopen();
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
    EXPECT_EQ(FxcCache::Find(KeyOf(1)), Bytes(100, 1));
    EXPECT_FALSE(FxcCache::Find(KeyOf(2)));
    EXPECT_FALSE(FxcCache::Find(KeyOf(3)));
    Reopen();
    EXPECT_EQ(ReadCache().size(), HEADER + RECORD + 100);
  }
}

TEST_F(FxcCacheTest, ForeignFileStartsOver)
{
  for (const std::string& foreign :
       {std::string("not a cache"), std::string("OFXC\x02\0\0\0", 8), std::string()})
  {
    WriteCache(foreign);
    EXPECT_FALSE(FxcCache::Find(KeyOf(1)));
    FxcCache::Store(KeyOf(1), Bytes(64, 9));
    Reopen();
    EXPECT_EQ(FxcCache::Find(KeyOf(1)), Bytes(64, 9));
    Reopen();
    EXPECT_EQ(ReadCache().size(), HEADER + RECORD + 64);
  }
}

TEST_F(FxcCacheTest, SharedWithAnotherOrca)
{
  // Another Orca on the same user folder has the file open to append the whole time this run reads
  // it and appends to it: a whole file, then a foreign one this run writes again from the start.
  for (const bool whole : {true, false})
  {
    if (whole)
    {
      FxcCache::Store(KeyOf(1), Bytes(100, 1));
      Reopen();
    }
    else
    {
      WriteCache("not a cache");
    }
    File::IOFile other(CachePath(), "ab", File::SharedAccess::ReadWrite);
    ASSERT_TRUE(other.IsOpen());
    EXPECT_FALSE(FxcCache::Find(KeyOf(2)));
    // The other Orca keeps a compile, then this run does: neither writes over the other.
    const std::string theirs = Record(KeyOf(2), Bytes(200, 2));
    ASSERT_TRUE(other.WriteBytes(theirs.data(), theirs.size()) && other.Flush());
    FxcCache::Store(KeyOf(3), Bytes(60, 3));
    other.Close();

    Reopen();
    EXPECT_EQ(FxcCache::Find(KeyOf(2)), Bytes(200, 2));
    EXPECT_EQ(FxcCache::Find(KeyOf(3)), Bytes(60, 3));
    EXPECT_EQ(FxcCache::Find(KeyOf(1)).has_value(), whole);
    Reopen();
    EXPECT_EQ(ReadCache().size(), HEADER + 2 * RECORD + 260 + (whole ? RECORD + 100 : 0));
  }
}
