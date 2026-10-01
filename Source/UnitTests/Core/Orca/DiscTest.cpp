// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Core/Orca/Disc.h"
#include "Core/Orca/Profile.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace
{
class FakeDevice final : public ciface::Core::Device
{
public:
  FakeDevice(std::string source, std::string name, int id)
      : m_source(std::move(source)), m_name(std::move(name))
  {
    SetId(id);
  }
  std::string GetName() const override { return m_name; }
  std::string GetSource() const override { return m_source; }

private:
  std::string m_source, m_name;
};
}  // namespace

TEST(OrcaDisc, MissingFileIsRefused)
{
  const Orca::DiscCheck check = Orca::CheckDisc("/nonexistent/brawl.iso", Orca::APP_GAME_ID);
  EXPECT_FALSE(check.ok);
  EXPECT_EQ(check.code, "disc_revision");
  EXPECT_FALSE(check.sentence.empty());
  EXPECT_FALSE(Orca::CheckDisc("", Orca::APP_GAME_ID).ok);
}

TEST(OrcaDisc, NonDiscFileIsRefused)
{
  const std::string path = File::CreateTempDir() + "/not-a-disc.iso";
  ASSERT_TRUE(File::WriteStringToFile(path, std::string(64 * 1024, 'x')));
  const Orca::DiscCheck check = Orca::CheckDisc(path, Orca::APP_GAME_ID);
  EXPECT_FALSE(check.ok);
  EXPECT_EQ(check.code, "disc_revision");
  File::DeleteDirRecursively(path.substr(0, path.rfind('/')));
}

// With ORCA_TEST_DISC set to the player's own Brawl image: it passes, and it isn't some other game.
TEST(OrcaDisc, TheRightDiscPasses)
{
  const char* disc = std::getenv("ORCA_TEST_DISC");
  if (!disc || !File::Exists(File::GetSysDirectory() + "Orca/RSBE01.ini"))
    GTEST_SKIP() << "set ORCA_TEST_DISC to a Brawl (USA, Rev 2) image";
  const Orca::DiscCheck check = Orca::CheckDisc(disc, Orca::APP_GAME_ID);
  EXPECT_TRUE(check.ok) << check.code << " " << check.sentence;
  const Orca::DiscCheck other = Orca::CheckDisc(disc, "RMCE01");
  EXPECT_FALSE(other.ok);
  EXPECT_EQ(other.code, "disc_revision");
}

TEST(OrcaDisc, SessionSaveSeedingCanBeTurnedOff)
{
  EXPECT_TRUE(Orca::SeedSessionSave());
  Orca::SetSeedSessionSave(false);
  EXPECT_FALSE(Orca::SeedSessionSave());
  Orca::SetSeedSessionSave(true);
}

// No save, or an empty data folder (what ES leaves at boot), is hash 0; any file makes it non-zero.
TEST(OrcaDisc, EmptySessionSaveHashesToZero)
{
  const std::string root = File::CreateTempDir() + "/";
  const std::string old_root = File::GetUserPath(D_SESSION_WIIROOT_IDX);
  File::SetUserPath(D_SESSION_WIIROOT_IDX, root);
  constexpr u64 brawl = 0x0001000052534245ull;
  EXPECT_EQ(Orca::HashSessionSave(brawl), 0u);
  const std::string data = root + "title/00010000/52534245/data/";
  ASSERT_TRUE(File::CreateFullPath(data));
  EXPECT_EQ(Orca::HashSessionSave(brawl), 0u);
  ASSERT_TRUE(File::WriteStringToFile(data + "save.bin", "x"));
  EXPECT_NE(Orca::HashSessionSave(brawl), 0u);
  File::SetUserPath(D_SESSION_WIIROOT_IDX, old_root);
  File::DeleteDirRecursively(root);
}

// The default pad mapping names "SDL/0/*": any model's first gamepad, never another source or id.
TEST(OrcaInput, AnyNameMatchesSourceAndId)
{
  ciface::Core::DeviceQualifier any;
  any.FromString("SDL/0/*");
  EXPECT_TRUE(any == static_cast<const ciface::Core::Device*>(
                         std::make_unique<FakeDevice>("SDL", "Xbox Wireless Controller", 0).get()));
  EXPECT_TRUE(any == static_cast<const ciface::Core::Device*>(
                         std::make_unique<FakeDevice>("SDL", "DualSense", 0).get()));
  EXPECT_FALSE(any == static_cast<const ciface::Core::Device*>(
                          std::make_unique<FakeDevice>("SDL", "DualSense", 1).get()));
  EXPECT_FALSE(any == static_cast<const ciface::Core::Device*>(
                          std::make_unique<FakeDevice>("Quartz", "Keyboard & Mouse", 0).get()));

  ciface::Core::DeviceQualifier exact;
  exact.FromString("SDL/0/DualSense");
  const auto xbox = std::make_unique<FakeDevice>("SDL", "Xbox Wireless Controller", 0);
  EXPECT_FALSE(exact == static_cast<const ciface::Core::Device*>(xbox.get()));
}
