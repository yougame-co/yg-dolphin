// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "AudioCommon/NullSoundStream.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/ScopeGuard.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/HW.h"
#include "Core/HW/Memmap.h"
#include "Core/Orca/Games/Brawl/BrawlModule.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Keyframe.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/UX.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/OnlineMatch.h"
#include "Core/Rollback/Rollback.h"
#include "Core/Rollback/ReplayStream.h"
#include "Core/System.h"
#include "UICommon/UICommon.h"
#include "VideoBackends/Null/VideoBackend.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
void SetSession(const std::string& value)
{
#ifdef _WIN32
  _putenv_s("ORCA_SESSION", value.c_str());
#else
  if (value.empty())
    unsetenv("ORCA_SESSION");
  else
    setenv("ORCA_SESSION", value.c_str(), 1);
#endif
}

// Real GC device state with null graphics and a temporary user root. No disc, IOS, audio device,
// controller backend, or room server is needed to exercise the local snapshot and boundary paths.
class OrcaSnapshot : public testing::Test
{
protected:
  void SetUp() override
  {
    m_dir = File::CreateTempDir();
    ASSERT_FALSE(m_dir.empty());
    m_session = Orca::GetEnv("ORCA_SESSION");
    SetSession("0");
    UICommon::SetUserDirectory(m_dir);
    Config::Init();
    SConfig::Init();
    Orca::Profile profile;
    profile.game_id = "GALE01";
    Config::AddLayer(Orca::GenerateConfigLoader(profile));
    Config::Set(Config::LayerType::Netplay, Config::MAIN_CPU_CORE, PowerPC::CPUCore::Interpreter);
    Core::DeclareAsCPUThread();
    m_cpu_thread = true;
    m_system.Initialize();
    m_wii = m_system.IsWii();
    m_system.SetIsWii(false);
    ASSERT_EQ(m_system.GetIOS(), nullptr);
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
  }

  void TearDown() override
  {
    Core::NotifyStateChanged(Core::State::Uninitialized);
    Orca::Status::SetOutput(nullptr);
    Orca::Status::SetAppCaps("");
    Orca::SetActiveProfile(std::nullopt);
    Orca::Engine::SetModule(nullptr);
    if (m_out)
      std::fclose(m_out);
    if (m_video)
      m_backend.Shutdown();
    g_video_backend = nullptr;
    if (m_hw)
      HW::Shutdown(m_system);
    m_system.SetSoundStream(nullptr);
    m_system.SetIsWii(m_wii);
    if (m_cpu_thread)
      Core::UndeclareAsCPUThread();
    SConfig::Shutdown();
    Config::Shutdown();
    SetSession(m_session);
    File::DeleteDirRecursively(m_dir);
  }

  std::string Lines()
  {
    std::rewind(m_out);
    char buffer[512];
    std::string lines;
    while (std::fgets(buffer, sizeof(buffer), m_out))
      lines += buffer;
    return lines;
  }

  void Boot(Orca::Profile profile)
  {
    SetSession("1");
    Orca::SetActiveProfile(std::move(profile));
    Orca::Engine::SetModule(&Orca::UX::Builtin());
    Orca::Status::SetAppCaps("join leave host direct");
  }

  Core::System& m_system = Core::System::GetInstance();
  Null::VideoBackend m_backend;
  std::string m_dir, m_session;
  std::FILE* m_out = nullptr;
  bool m_wii = false, m_hw = false, m_video = false, m_cpu_thread = false;
};
}  // namespace

TEST_F(OrcaSnapshot, GameCubeCaptureRestoreAndKeyframeLoadWithoutIos)
{
  auto& memory = m_system.GetMemory();
  auto& cpu = m_system.GetPPCState();
  ASSERT_NE(memory.GetRAM(), nullptr);
  ASSERT_EQ(memory.GetEXRAM(), nullptr);
  memory.GetRAM()[123] = 42;
  memory.GetL1Cache()[456] = 17;
  cpu.gpr[3] = 0x12345678;
  Rollback::MachineImage origin;
  ASSERT_TRUE(Rollback::SnapshotRing::Capture(m_system, &origin, true));
  ASSERT_FALSE(origin.state.empty());
  EXPECT_TRUE(origin.mem2.empty());
  const u64 hash = Rollback::RamChecksum(origin.mem1, origin.mem2);
  Rollback::SnapshotRing ring(3);
  ring.Save(m_system, 100);
  memory.GetRAM()[123] = 9;
  memory.GetL1Cache()[456] = 8;
  cpu.gpr[3] = 7;
  ASSERT_TRUE(ring.Load(m_system, 100));
  EXPECT_EQ(memory.GetRAM()[123], 42);
  EXPECT_EQ(memory.GetL1Cache()[456], 17);
  EXPECT_EQ(cpu.gpr[3], 0x12345678u);

  // The wire keyframe contains a replay; rebuilding starts from this process's own GC image.
  Orca::Net::ReplayArchive replay;
  replay.first_frame = 100;
  replay.origin_hash = hash;
  replay.frames.resize(2);
  const auto ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
      std::vector<Orca::Events::PortInfo>{{0, "host", false, {}, {}}});
  for (auto& record : replay.frames)
  {
    record.ports = ports;
    record.pads = Rollback::OnlineMatch::CanonicalBootPads();
  }
  replay.boundary.ports = ports;
  replay.boundary.pads = Rollback::OnlineMatch::CanonicalBootPads();
  const auto wire = Orca::Net::PackKeyframe(102, replay);
  ASSERT_FALSE(wire.empty());
  int frame = -1;
  Orca::Net::ReplayArchive loaded;
  ASSERT_TRUE(Orca::Net::UnpackKeyframe(wire, &frame, &loaded));
  EXPECT_EQ(frame, 102);
  EXPECT_EQ(loaded.first_frame, 100);
  EXPECT_EQ(loaded.origin_hash, hash);
  memory.GetRAM()[123] = 0;
  cpu.gpr[3] = 0;
  ASSERT_TRUE(ring.LoadMachineImage(m_system, std::move(origin), loaded.first_frame));
  EXPECT_EQ(memory.GetRAM()[123], 42);
  EXPECT_EQ(cpu.gpr[3], 0x12345678u);
  EXPECT_EQ(ring.RamChecksum(100), hash);
  EXPECT_EQ(m_system.GetIOS(), nullptr);
  ring.Reset(m_system);
}

TEST_F(OrcaSnapshot, BoundarySwitchesRuleBeforeTheFirstKeyframeHook)
{
  Orca::Profile profile;
  profile.game_id = "RSBE01";
  profile.revision = 2;
  profile.mod.emplace();
  profile.mod->manifest.builtin = Orca::Mod::Builtin::BrawlRev2;
  profile.mod->manifest.first_keyframe_frame = 3;
  profile.mod->manifest.origin = Orca::Mod::OriginDecl{3, {}, 30};
  Boot(profile);
  GCPadStatus pressed;
  pressed.button = PAD_BUTTON_A;
  for (int boundary = 0; boundary <= 4; ++boundary)
  {
    Rollback::OnlineMatch::OnBoundary(m_system,
        [&](int) { return Orca::Net::EncodePad(pressed); },
        [&](int frame, bool, const auto&, bool) {
          EXPECT_EQ(frame, boundary);
          // These checks run before the module hook has refreshed any guard verdict.
          EXPECT_EQ(Orca::Mod::GuardsFailed(), frame >= 3);
          EXPECT_EQ(Orca::Mod::GuardsDecided(), frame >= 3);
        });
    const auto pad = Rollback::RawInputOverride(0);
    ASSERT_TRUE(pad);
    EXPECT_EQ(pad->button, boundary >= 3 ? PAD_BUTTON_A : 0);
  }
  EXPECT_TRUE(Orca::Status::Cap("join"));
  EXPECT_EQ(Lines().find("no_origin"), std::string::npos);
}

TEST_F(OrcaSnapshot, LegacyMenuDeadlineDoesNotPrintAPackReport)
{
  Orca::Profile profile;
  profile.game_id = "RSBE01";
  profile.revision = 2;
  Boot(profile);
  for (int boundary = 0; boundary <= Rollback::OnlineMatch::ORIGIN_DEADLINE; ++boundary)
    Rollback::OnlineMatch::OnBoundary(m_system, [](int) { return Orca::Net::Pad{}; },
                                    [](int, bool, const auto&, bool) {});
  EXPECT_FALSE(Orca::Status::Cap("join"));
  EXPECT_EQ(Lines().find("no_origin"), std::string::npos);
  EXPECT_EQ(Lines().find("This pack's game"), std::string::npos);
  EXPECT_EQ(Orca::Status::ExitCode(), 0);
}

TEST_F(OrcaSnapshot, RecordedReplayChecksEveryHashWithAndWithoutViewNames)
{
  SetSession("1");
  m_system.GetPPCState().msr.DR = 0;
  const std::string source_before = Orca::GetEnv("ORCA_REPLAY_SOURCE");
  const std::string names_before = Orca::GetEnv("ORCA_REPLAY_NAMES");
  const auto set = [](const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    if (value.empty())
      unsetenv(key);
    else
      setenv(key, value.c_str(), 1);
#endif
  };
  const Common::ScopeGuard restore([&] {
    set("ORCA_REPLAY_SOURCE", source_before);
    set("ORCA_REPLAY_NAMES", names_before);
  });
  auto& ram = m_system.GetMemory();
  const std::vector<u8> origin(ram.GetRAM(), ram.GetRAM() + ram.GetRamSize());
  const u64 origin_hash = Rollback::RamChecksum(origin, {});
  const Core::CPUThreadGuard guard(m_system);
  Orca::UX::GuardMemory memory(guard);
  ASSERT_TRUE(memory.Valid(0x1000));
  std::string shown;
  const Rollback::OnlineMatch::FrameHook hook = [&](int frame, bool, const auto& ports, bool) {
    shown = ports.at(0).name;
    for (size_t i = 0; i <= shown.size(); ++i)
      Rollback::ReplayText::Write8(memory, 0x1000 + static_cast<u32>(i),
                                   i < shown.size() ? static_cast<u8>(shown[i]) : 0);
    memory.Write32(0x2000, static_cast<u32>(frame));
  };
  Orca::Net::ReplayArchive replay;
  replay.first_frame = 0;
  replay.origin_hash = origin_hash;
  auto ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
      std::vector<Orca::Events::PortInfo>{{0, "Alice", false}});
  for (int frame = 0; frame < 121; ++frame)
  {
    Orca::Net::ReplayFrame record;
    record.ports = ports;
    replay.frames.push_back(record);
    const Rollback::ReplayText::Scope scope(replay.text);
    hook(frame, false, *ports, false);
    if (frame % 60 == 0)
      replay.checksums[frame] = replay.text.Checksum({ram.GetRAM(), ram.GetRamSize()}, {});
  }
  EXPECT_EQ(shown, "Alice");
  const std::string base = m_dir + "/own";
  {
    Rollback::ReplayStream::Writer writer(base);
    ASSERT_TRUE(writer.Pump(replay, 121, 121, &replay.checksums));
  }
  set("ORCA_REPLAY_SOURCE", base + "-1.ygr");
  for (const bool view_names : {false, true})
  {
    SCOPED_TRACE(view_names);
    set("ORCA_REPLAY_NAMES", view_names ? "0 Viewer%20Name" : "");
    auto player = Rollback::ReplayStream::Player::FromEnv();
    ASSERT_NE(player, nullptr);
    Rollback::ReplayStream::PlayerHost host;
    host.restore_origin = [&] {
      std::copy(origin.begin(), origin.end(), ram.GetRAM());
      return true;
    };
    host.set_catching_up = [](bool) {};
    host.set_pads = [](int, const auto&) {};
    host.stopping = [] { return false; };
    std::string stopped;
    host.stop = [&](const std::string& message) { stopped = message; };
    int running = -1;
    for (int tick = 0; tick < 123 && !player->Over(); ++tick)
      player->Step(m_system, host, &running, origin_hash, 0, hook);
    EXPECT_TRUE(player->Over());
    EXPECT_EQ(stopped, "The replay is over");
    EXPECT_EQ(shown, view_names ? "Viewer Name" : "P1");
  }
  const std::string lines = Lines();
  const std::string verdict = "orca replay done ok frames=121 checked=2 mismatched=0";
  const size_t first = lines.find(verdict);
  ASSERT_NE(first, std::string::npos) << lines;
  EXPECT_NE(lines.find(verdict, first + verdict.size()), std::string::npos) << lines;
}
