// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Rollback loads keep compiled JIT code unless a restored word that differs was compiled, or the
// JIT dropped code or learned facts since the snapshot (Rollback.h CodeKeepCheck). Run over both
// snapshot kinds: copy-on-write (a session) and full copies.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "AudioCommon/NullSoundStream.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FPURoundMode.h"
#include "Common/FileUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/HW.h"
#include "Core/HW/Memmap.h"
#include "Core/MemTools.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/Cow.h"
#include "Core/Rollback/Rollback.h"
#include "Core/System.h"
#include "UICommon/UICommon.h"
#include "VideoBackends/Null/VideoBackend.h"
#include "VideoCommon/VideoConfig.h"

// After the JIT headers: x64Emitter declares a TEST member that gtest's macro would rewrite.
#include <gtest/gtest.h>

namespace
{
#if defined(_M_X86_64)
constexpr PowerPC::CPUCore HOST_JIT = PowerPC::CPUCore::JIT64;
#else
constexpr PowerPC::CPUCore HOST_JIT = PowerPC::CPUCore::JITARM64;
#endif

constexpr u32 ADDI_R3_1 = 0x38630001;  // addi r3, r3, 1
constexpr u32 ADDI_R3_2 = 0x38630002;  // addi r3, r3, 2
constexpr u32 BRANCH_TO_SELF = 0x48000000;
constexpr u32 CACHED = 0x80000000;  // MEM1 through BAT 0: physical | 0x80000000

// Block A: three adds and a branch at 0x00100000..0x00100010. Block B in another page.
constexpr u32 CODE_A = 0x00100000;
constexpr u32 CODE_B = 0x00300000;
// Data next to A's code in the same 32-byte line, in A's 4 KB page, and in a page of its own.
constexpr u32 DATA_SAME_LINE = 0x00100018;
constexpr u32 DATA_SAME_PAGE = 0x00100800;
constexpr u32 DATA_OWN_PAGE = 0x00200000;

void SetEnv(const char* name, const std::string& value)
{
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  if (value.empty())
    unsetenv(name);
  else
    setenv(name, value.c_str(), 1);
#endif
}

// A GameCube with null graphics and the host's JIT, running translated through BAT 0 as games do.
// The parameter picks copy-on-write snapshots (true) or full copies.
class KeepJitLoad : public testing::TestWithParam<bool>
{
protected:
  void SetUp() override
  {
    m_dir = File::CreateTempDir();
    ASSERT_FALSE(m_dir.empty());
    m_session = Orca::GetEnv("ORCA_SESSION");
    SetEnv("ORCA_SESSION", "0");
    UICommon::SetUserDirectory(m_dir);
    Config::Init();
    SConfig::Init();
    Orca::Profile profile;
    profile.game_id = "GALE01";
    Config::AddLayer(Orca::GenerateConfigLoader(profile));
    Config::SetCurrent(Config::MAIN_CPU_CORE, HOST_JIT);
    Config::SetCurrent(Config::MAIN_FASTMEM, false);
    Config::SetCurrent(Config::MAIN_FASTMEM_ARENA, false);
    Core::DeclareAsCPUThread();
    m_cpu_thread = true;
    m_system.Initialize();
    m_wii = m_system.IsWii();
    m_system.SetIsWii(false);
    m_system.SetSoundStream(std::make_unique<NullSound>());
    HW::Init(m_system, Orca::SessionSram());
    m_hw = true;
    g_video_backend = &m_backend;
    m_backend.InitBackendInfo({});
    g_Config.Refresh();
    ASSERT_TRUE(m_backend.Initialize({}));
    m_video = true;
    m_out = std::tmpfile();
    ASSERT_NE(m_out, nullptr);
    Orca::Status::SetOutput(m_out);
    ASSERT_NE(m_system.GetJitInterface().GetCore(), nullptr);

    // BAT 0 maps 0x80000000 to physical 0 (256 MB), instructions and data, as the GameCube's OS
    // sets it up.
    auto& ppc = m_system.GetPPCState();
    ppc.spr[SPR_IBAT0U] = ppc.spr[SPR_DBAT0U] = 0x80001FFF;
    ppc.spr[SPR_IBAT0L] = ppc.spr[SPR_DBAT0L] = 0x00000002;
    m_system.GetMMU().IBATUpdated();
    m_system.GetMMU().DBATUpdated();
    ppc.msr.Hex = 0;
    ppc.msr.IR = 1;
    ppc.msr.DR = 1;
    m_system.GetPowerPC().MSRUpdated();

    if (GetParam())
    {
      if (!EMM::IsExceptionHandlerSupported())
        GTEST_SKIP() << "no fault handler on this platform";
      EMM::InstallExceptionHandler();
      m_handler = true;
      SetEnv("ORCA_SESSION", "1");
    }

    WriteBlock(CODE_A, ADDI_R3_1);
    WriteBlock(CODE_B, ADDI_R3_1);
    m_system.GetMemory().Write_U32(0x11111111, DATA_SAME_LINE);
    m_system.GetMemory().Write_U32(0x22222222, DATA_SAME_PAGE);
    m_system.GetMemory().Write_U32(0x33333333, DATA_OWN_PAGE);
  }

  void TearDown() override
  {
    m_ring.reset();
    Core::NotifyStateChanged(Core::State::Uninitialized);
    Orca::Status::SetOutput(nullptr);
    if (m_out)
      std::fclose(m_out);
    if (m_video)
      m_backend.Shutdown();
    g_video_backend = nullptr;
    if (m_hw)
      HW::Shutdown(m_system);
    if (m_handler)
      EMM::UninstallExceptionHandler();
    m_system.SetSoundStream(nullptr);
    m_system.SetIsWii(m_wii);
    if (m_cpu_thread)
      Core::UndeclareAsCPUThread();
    SConfig::Shutdown();
    Config::Shutdown();
    Common::FPU::LoadDefaultSIMDState();
    SetEnv("ORCA_SESSION", m_session);
    File::DeleteDirRecursively(m_dir);
  }

  JitBase& Jit() { return *static_cast<JitBase*>(m_system.GetJitInterface().GetCore()); }

  // Three adds of `add` and a branch to the first.
  void WriteBlock(u32 physical, u32 add)
  {
    auto& memory = m_system.GetMemory();
    for (u32 i = 0; i < 3; ++i)
      memory.Write_U32(add, physical + i * 4);
    memory.Write_U32(BRANCH_TO_SELF, physical + 12);
  }

  JitBlock* Compile(u32 physical)
  {
    Jit().Jit(physical | CACHED);
    return Find(physical);
  }

  JitBlock* Find(u32 physical)
  {
    return Jit().GetBlockCache()->GetBlockFromStartAddress(physical | CACHED,
                                                           m_system.GetPPCState().feature_flags);
  }

  u32 Read(u32 physical) { return m_system.GetMemory().Read_U32(physical); }

  void Save(s64 frame)
  {
    if (!m_ring)
      m_ring = std::make_unique<Rollback::SnapshotRing>(4);
    m_ring->Save(m_system, frame);
    // The parameter must have picked the snapshot kind it names.
    ASSERT_EQ(Rollback::Cow::IsArmedFor(m_ring.get()), GetParam());
  }

  void Load(s64 frame) { ASSERT_TRUE(m_ring->Load(m_system, frame)); }

  Core::System& m_system = Core::System::GetInstance();
  Null::VideoBackend m_backend;
  std::unique_ptr<Rollback::SnapshotRing> m_ring;
  std::string m_dir, m_session;
  std::FILE* m_out = nullptr;
  bool m_wii = false, m_hw = false, m_video = false, m_cpu_thread = false, m_handler = false;
};

std::string KindName(const testing::TestParamInfo<bool>& info)
{
  return info.param ? "CopyOnWrite" : "FullCopy";
}
}  // namespace

TEST_P(KeepJitLoad, NoChangeKeepsEveryBlock)
{
  JitBlock* const a = Compile(CODE_A);
  ASSERT_NE(a, nullptr);
  Save(10);
  const u64 generation = JitInterface::CodeGeneration();
  Load(10);
  EXPECT_EQ(Find(CODE_A), a);
  EXPECT_EQ(JitInterface::CodeGeneration(), generation);
}

TEST_P(KeepJitLoad, DataOnlyWritesKeepEveryBlock)
{
  JitBlock* const a = Compile(CODE_A);
  ASSERT_NE(a, nullptr);
  Save(10);
  // A block compiled after the snapshot from words the restore leaves alone stays too.
  JitBlock* const b = Compile(CODE_B);
  ASSERT_NE(b, nullptr);
  // Data beside A's code in its own 32-byte line, elsewhere in its page, and in another page:
  // the old rule dropped A here, because its page changed.
  auto& memory = m_system.GetMemory();
  memory.Write_U32(0xAAAAAAAA, DATA_SAME_LINE);
  memory.Write_U32(0xBBBBBBBB, DATA_SAME_PAGE);
  memory.Write_U32(0xCCCCCCCC, DATA_OWN_PAGE);
  const u64 generation = JitInterface::CodeGeneration();
  Load(10);
  EXPECT_EQ(Read(DATA_SAME_LINE), 0x11111111u);
  EXPECT_EQ(Read(DATA_SAME_PAGE), 0x22222222u);
  EXPECT_EQ(Read(DATA_OWN_PAGE), 0x33333333u);
  EXPECT_EQ(Find(CODE_A), a);
  EXPECT_EQ(Find(CODE_B), b);
  EXPECT_EQ(JitInterface::CodeGeneration(), generation);

  // Again, from the same snapshot.
  memory.Write_U32(0xDDDDDDDD, DATA_SAME_PAGE);
  Load(10);
  EXPECT_EQ(Read(DATA_SAME_PAGE), 0x22222222u);
  EXPECT_EQ(Find(CODE_A), a);
}

TEST_P(KeepJitLoad, AChangedCodeWordDropsTheChangedPages)
{
  JitBlock* const a = Compile(CODE_A);
  JitBlock* const b = Compile(CODE_B);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  Save(10);
  // A's second instruction is rewritten (no icbi, so the JIT doesn't know), and data elsewhere.
  auto& memory = m_system.GetMemory();
  memory.Write_U32(ADDI_R3_2, CODE_A + 4);
  memory.Write_U32(0xCCCCCCCC, DATA_OWN_PAGE);
  Load(10);
  EXPECT_EQ(Read(CODE_A + 4), ADDI_R3_1);
  EXPECT_EQ(Read(DATA_OWN_PAGE), 0x33333333u);
  // As before this change: the blocks of every changed page go, the rest stay.
  EXPECT_EQ(Find(CODE_A), nullptr);
  EXPECT_EQ(Find(CODE_B), b);

  // A block compiled after the snapshot from a word that differs from it goes too, even with the
  // JIT's generation unchanged since that load.
  memory.Write_U32(ADDI_R3_2, CODE_A + 4);
  ASSERT_NE(Compile(CODE_A), nullptr);
  Load(10);
  EXPECT_EQ(Read(CODE_A + 4), ADDI_R3_1);
  EXPECT_EQ(Find(CODE_A), nullptr);
  EXPECT_EQ(Find(CODE_B), b);
}

TEST_P(KeepJitLoad, ACacheFlushSinceTheSnapshotDropsTheChangedPages)
{
  JitBlock* const a = Compile(CODE_A);
  ASSERT_NE(Compile(CODE_B), nullptr);
  ASSERT_NE(a, nullptr);
  Save(10);
  // The game flushes B's line from the instruction cache (icbi): the JIT lost a block.
  m_system.GetJitInterface().InvalidateICacheLine(CODE_B | CACHED);
  ASSERT_EQ(Find(CODE_B), nullptr);
  auto& memory = m_system.GetMemory();
  memory.Write_U32(0xBBBBBBBB, DATA_SAME_PAGE);
  Load(10);
  EXPECT_EQ(Read(DATA_SAME_PAGE), 0x22222222u);
  // Not kept: A's page changed, so A goes, as before this change.
  EXPECT_EQ(Find(CODE_A), nullptr);

  // That load brought the JIT in line with the snapshot, so the next load of it keeps code again.
  JitBlock* const a2 = Compile(CODE_A);
  ASSERT_NE(a2, nullptr);
  memory.Write_U32(0xBBBBBBBB, DATA_SAME_PAGE);
  Load(10);
  EXPECT_EQ(Find(CODE_A), a2);

  // A whole-cache clear since the snapshot counts the same way.
  {
    const Core::CPUThreadGuard guard(m_system);
    m_system.GetJitInterface().ClearCache(guard);
  }
  JitBlock* const a3 = Compile(CODE_A);
  ASSERT_NE(a3, nullptr);
  memory.Write_U32(0xBBBBBBBB, DATA_SAME_PAGE);
  Load(10);
  EXPECT_EQ(Find(CODE_A), nullptr);
}

TEST_P(KeepJitLoad, ALearnedFactSinceTheSnapshotDropsTheChangedPages)
{
  ASSERT_NE(Compile(CODE_A), nullptr);
  Save(10);
  // The JIT learns that A's start needs its GQR guess checked; it recompiles A with that.
  const u64 generation = JitInterface::CodeGeneration();
  auto& ppc = m_system.GetPPCState();
  const u32 pc = ppc.pc;
  ppc.pc = CODE_A | CACHED;
  m_system.GetJitInterface().CompileExceptionCheck(JitInterface::ExceptionType::PairedQuantize);
  ppc.pc = pc;
  EXPECT_NE(JitInterface::CodeGeneration(), generation);
  JitBlock* const a = Compile(CODE_A);
  ASSERT_NE(a, nullptr);
  m_system.GetMemory().Write_U32(0xBBBBBBBB, DATA_SAME_PAGE);
  Load(10);
  EXPECT_EQ(Find(CODE_A), nullptr);
}

TEST_P(KeepJitLoad, YgKeepJitZeroRestoresTheOldRule)
{
  // Read once per process, so only check it when the run set it.
  const char* value = std::getenv("YG_KEEPJIT");
  if (!value || std::string(value) != "0")
    GTEST_SKIP() << "YG_KEEPJIT=0 not set";
  ASSERT_NE(Compile(CODE_A), nullptr);
  Save(10);
  m_system.GetMemory().Write_U32(0xBBBBBBBB, DATA_SAME_PAGE);
  Load(10);
  EXPECT_EQ(Find(CODE_A), nullptr);
}

// What the comparison adds to a load, against what it saves: compiling blocks again. Worst case:
// every 32-byte line of every restored 4 KB block differs, half of them in pages with code.
TEST_P(KeepJitLoad, TheComparisonCostsFarLessThanTheRecompiles)
{
  if (GetParam())
    GTEST_SKIP() << "measured once, without copy-on-write";
  constexpr u32 BLOCKS = 256;
  constexpr u32 CODE_PAGES = 0x00400000;
  constexpr u32 PAGE = 4096;
  const auto us = [](Clock::duration d) {
    return std::chrono::duration<double, std::micro>(d).count();
  };

  // One compiled block at the start of each of the first BLOCKS / 2 pages.
  for (u32 i = 0; i < BLOCKS / 2; ++i)
    WriteBlock(CODE_PAGES + i * PAGE, ADDI_R3_1);
  const auto compile_start = Clock::now();
  for (u32 i = 0; i < BLOCKS / 2; ++i)
    Jit().Jit((CODE_PAGES + i * PAGE) | CACHED);
  const double compile_us = us(Clock::now() - compile_start) / (BLOCKS / 2);
  for (u32 i = 0; i < BLOCKS / 2; ++i)
    ASSERT_NE(Find(CODE_PAGES + i * PAGE), nullptr);

  // Every line differs in its last word; the code words (first 16 bytes of a page) never do.
  std::vector<u8> live(BLOCKS * PAGE, 0), saved(BLOCKS * PAGE, 0);
  for (u32 at = 28; at < BLOCKS * PAGE; at += 32)
    saved[at] = 1;
  Rollback::CodeKeepCheck check(m_system.GetJitInterface(), true);
  const auto compare_start = Clock::now();
  for (u32 i = 0; i < BLOCKS; ++i)
  {
    check.Compare(CODE_PAGES + i * PAGE, live.data() + i * PAGE, saved.data() + i * PAGE, PAGE);
  }
  const double compare_us = us(Clock::now() - compare_start) / BLOCKS;
  EXPECT_TRUE(check.Keep());

  // And the word that is code does count.
  saved[4] = 1;
  Rollback::CodeKeepCheck code_changed(m_system.GetJitInterface(), true);
  code_changed.Compare(CODE_PAGES, live.data(), saved.data(), PAGE);
  EXPECT_FALSE(code_changed.Keep());

  std::printf("[ keepjit  ] compare %.3f us per changed 4 KB block (every line changed); "
              "compile %.1f us per block\n",
              compare_us, compile_us);
  RecordProperty("compare_ns_per_4k_block", static_cast<int>(compare_us * 1000));
  RecordProperty("compile_ns_per_block", static_cast<int>(compile_us * 1000));
  // A loose bound, far from both numbers, so only a broken query trips it.
  EXPECT_LT(compare_us, compile_us);
}

// The prefix leads the full test names, so it starts with Orca for the Orca* filters.
INSTANTIATE_TEST_SUITE_P(OrcaKeepJit, KeepJitLoad, testing::Bool(), KindName);
