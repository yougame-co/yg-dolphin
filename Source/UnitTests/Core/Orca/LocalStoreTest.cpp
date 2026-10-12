// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// What Orca keeps between runs (Core/Orca/LocalStore.h): the one-time "seen" keys. Game modules ask
// the store and never open a file themselves.

#include <string>

#include <gtest/gtest.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Core/Orca/LocalStore.h"

namespace LocalStore = Orca::LocalStore;

TEST(OrcaLocalStore, TheSeenFileKeepsOneKeyPerLine)
{
  constexpr std::string_view key = "hold-b-leave";
  EXPECT_FALSE(LocalStore::SeenIn("", key));
  EXPECT_FALSE(LocalStore::SeenIn("hold-b-leaves\n", key));
  EXPECT_TRUE(LocalStore::SeenIn("hold-b-leave", key));
  EXPECT_TRUE(LocalStore::SeenIn("other\r\n  hold-b-leave \r\n", key));
  EXPECT_EQ(LocalStore::SeenWith("", key), "hold-b-leave\n");
  EXPECT_EQ(LocalStore::SeenWith("other", key), "other\nhold-b-leave\n");
  // Idempotent: a key already there is kept as is.
  EXPECT_EQ(LocalStore::SeenWith("hold-b-leave\nother\n", key), "hold-b-leave\nother\n");
  EXPECT_TRUE(LocalStore::SeenIn(LocalStore::SeenWith("a\nb\n", key), key));
}

// MarkSeen adds to what the file holds (another run may have kept other keys), and Seen answers
// from it afterwards, in the user folder's Config.
TEST(OrcaLocalStore, MarkSeenKeepsTheKeyBesideTheOthers)
{
  const std::string old_user = File::GetUserPath(D_USER_IDX);
  const std::string dir = File::CreateTempDir();
  File::SetUserPath(D_USER_IDX, dir + DIR_SEP);
  ASSERT_TRUE(File::CreateFullPath(File::GetUserPath(D_CONFIG_IDX)));
  const std::string path = LocalStore::SeenPath();
  EXPECT_EQ(path, File::GetUserPath(D_CONFIG_IDX) + "OrcaSeen.txt");
  ASSERT_TRUE(File::WriteStringToFile(path, "other"));
  EXPECT_TRUE(LocalStore::MarkSeen("hold-b-leave"));
  std::string text;
  ASSERT_TRUE(File::ReadFileToString(path, text));
  EXPECT_EQ(text, "other\nhold-b-leave\n");
  EXPECT_TRUE(LocalStore::Seen("hold-b-leave"));
  EXPECT_TRUE(LocalStore::Seen("other"));
  EXPECT_FALSE(LocalStore::Seen("hold-b"));
  // Kept again: the file is left as it is.
  EXPECT_TRUE(LocalStore::MarkSeen("other"));
  ASSERT_TRUE(File::ReadFileToString(path, text));
  EXPECT_EQ(text, "other\nhold-b-leave\n");
  File::SetUserPath(D_USER_IDX, old_user);
  File::DeleteDirRecursively(dir);
}
