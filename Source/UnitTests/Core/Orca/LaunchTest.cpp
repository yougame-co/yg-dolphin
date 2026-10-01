// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Core/Orca/Launch.h"

namespace
{
constexpr char GOOD[] = "# Project+ v3.2\n"
                        "[Launch]\n"
                        "Profile = PPLUS32\n"
                        "Executable = pplus/Project+ Netplay Launcher.dol\r\n";

bool Refused(const std::string& text)
{
  std::string error;
  const bool refused = !Orca::ParseLaunchIni(text, &error).has_value();
  return refused && !error.empty();
}
}  // namespace

TEST(OrcaLaunch, ParsesTheTwoKeys)
{
  std::string error;
  const auto spec = Orca::ParseLaunchIni(GOOD, &error);
  ASSERT_TRUE(spec) << error;
  EXPECT_EQ(spec->profile, "PPLUS32");
  EXPECT_EQ(spec->executable, "pplus/Project+ Netplay Launcher.dol");
}

TEST(OrcaLaunch, RefusesAnythingElse)
{
  EXPECT_TRUE(Refused(""));
  EXPECT_TRUE(Refused("[Launch]\nProfile = PPLUS32\n"));
  EXPECT_TRUE(Refused("[Launch]\nExecutable = a.dol\n"));
  EXPECT_TRUE(Refused("Profile = PPLUS32\n[Launch]\nExecutable = a.dol\n"));
  EXPECT_TRUE(Refused("[Other]\nProfile = PPLUS32\nExecutable = a.dol\n"));
  EXPECT_TRUE(Refused("[Launch]\n[Launch]\nProfile = PPLUS32\nExecutable = a.dol\n"));
  EXPECT_TRUE(Refused("[Launch]\nProfile = PPLUS32\nExecutable = a.dol\nSDImage = sd.raw\n"));
  EXPECT_TRUE(Refused("[Launch]\nProfile = PPLUS32\nProfile = RSBE01\nExecutable = a.dol\n"));
  EXPECT_TRUE(Refused("[Launch]\nProfile = PPLUS32\nExecutable\n"));
  EXPECT_TRUE(Refused("[Launch]\nProfile = ../PPLUS32\nExecutable = a.dol\n"));
  EXPECT_TRUE(Refused("[Launch]\nProfile = \nExecutable = a.dol\n"));
  EXPECT_TRUE(Refused(std::string("[Launch]\nProfile = PPLUS32\nExecutable = a.dol\n#") +
                      std::string(5000, 'x')));
}

TEST(OrcaLaunch, PathsStayInsideTheFolder)
{
  for (const char* path : {"/etc/passwd", "../a.dol", "pplus/../../a.dol", "pplus/./a.dol", "./a.dol",
                           "pplus//a.dol", "pplus/", "pplus\\a.dol", "pplus/..", ".."})
  {
    EXPECT_TRUE(Refused(std::string("[Launch]\nProfile = PPLUS32\nExecutable = ") + path + "\n"))
        << path;
  }
}

TEST(OrcaLaunch, ReadsTheFileNextToTheApp)
{
  const std::string dir = File::CreateTempDir();
  std::string error;
  // No file: a plain Orca.app, not an error.
  EXPECT_FALSE(Orca::ReadLaunchIni(dir, &error));
  EXPECT_TRUE(error.empty());

  ASSERT_TRUE(File::WriteStringToFile(dir + "/orca-launch.ini", GOOD));
  // The loader isn't there yet.
  EXPECT_FALSE(Orca::ReadLaunchIni(dir, &error));
  EXPECT_FALSE(error.empty());

  ASSERT_TRUE(File::CreateFullPath(dir + "/pplus/"));
  ASSERT_TRUE(File::WriteStringToFile(dir + "/pplus/Project+ Netplay Launcher.dol", "dol"));
  const auto spec = Orca::ReadLaunchIni(dir, &error);
  ASSERT_TRUE(spec) << error;
  EXPECT_EQ(spec->profile, "PPLUS32");
  // The loader's full path (separators and drive letters differ by platform): absolute, and the
  // file written above.
  const std::filesystem::path executable(spec->executable);
  EXPECT_TRUE(executable.is_absolute()) << spec->executable;
  EXPECT_EQ(executable.filename().string(), "Project+ Netplay Launcher.dol");
  std::error_code ec;
  EXPECT_TRUE(std::filesystem::equivalent(
      executable, std::filesystem::path(dir) / "pplus" / "Project+ Netplay Launcher.dol", ec))
      << spec->executable << " " << ec.message();

#ifndef _WIN32
  // A symlink that leads out of the folder is refused like a ".." path.
  const std::string outside = File::CreateTempDir();
  ASSERT_TRUE(File::WriteStringToFile(outside + "/evil.dol", "x"));
  File::Delete(dir + "/pplus/Project+ Netplay Launcher.dol");
  ASSERT_EQ(symlink((outside + "/evil.dol").c_str(),
                    (dir + "/pplus/Project+ Netplay Launcher.dol").c_str()),
            0);
  EXPECT_FALSE(Orca::ReadLaunchIni(dir, &error));
  EXPECT_NE(error.find("outside"), std::string::npos) << error;
  File::DeleteDirRecursively(outside);
#endif

  File::DeleteDirRecursively(dir);
}
