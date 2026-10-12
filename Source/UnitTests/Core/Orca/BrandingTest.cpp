// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "Core/Orca/Branding.h"

using Orca::BrandWords;

TEST(OrcaBranding, ProductName)
{
  EXPECT_EQ(Orca::PRODUCT_NAME, "YG Baiji");
}

TEST(OrcaBranding, ReplacesEachWord)
{
  EXPECT_EQ(BrandWords("Dolphin"), "YG Baiji");
  EXPECT_EQ(BrandWords("Orca"), "YG Baiji");
  EXPECT_EQ(BrandWords("Dolphin has stopped."), "YG Baiji has stopped.");
  EXPECT_EQ(BrandWords("Orca couldn't start the game"), "YG Baiji couldn't start the game");
}

TEST(OrcaBranding, ReplacesBothWords)
{
  EXPECT_EQ(BrandWords("Orca is a fork of Dolphin"), "YG Baiji is a fork of YG Baiji");
  EXPECT_EQ(BrandWords("Dolphin, Orca; Dolphin"), "YG Baiji, YG Baiji; YG Baiji");
}

TEST(OrcaBranding, WordEdges)
{
  // Punctuation around a word, and the end of a sentence.
  EXPECT_EQ(BrandWords("Dolphin's settings"), "YG Baiji's settings");
  EXPECT_EQ(BrandWords("Reset Orca's files"), "Reset YG Baiji's files");
  EXPECT_EQ(BrandWords("Please restart Dolphin."), "Please restart YG Baiji.");
  EXPECT_EQ(BrandWords("(Dolphin)"), "(YG Baiji)");
  EXPECT_EQ(BrandWords("\"Orca\""), "\"YG Baiji\"");
  EXPECT_EQ(BrandWords("Dolphin\nOrca"), "YG Baiji\nYG Baiji");
  EXPECT_EQ(BrandWords("Orca: boot"), "YG Baiji: boot");
}

TEST(OrcaBranding, LeavesOtherWords)
{
  EXPECT_EQ(BrandWords(""), "");
  EXPECT_EQ(BrandWords("DolphinNoGUI"), "DolphinNoGUI");
  EXPECT_EQ(BrandWords("OrcaControls-RSBE01.txt"), "OrcaControls-RSBE01.txt");
  EXPECT_EQ(BrandWords("Orcas"), "Orcas");
  EXPECT_EQ(BrandWords("Orca2"), "Orca2");
  EXPECT_EQ(BrandWords("Dolphin_Emu"), "Dolphin_Emu");
  EXPECT_EQ(BrandWords("dolphin and orca"), "dolphin and orca");
  EXPECT_EQ(BrandWords("SuperDolphin"), "SuperDolphin");
}

TEST(OrcaBranding, LeavesPathsAndFileNames)
{
  EXPECT_EQ(BrandWords("/Users/me/Library/Application Support/Dolphin/Config"),
            "/Users/me/Library/Application Support/Dolphin/Config");
  EXPECT_EQ(BrandWords("C:\\Games\\Orca\\Sys"), "C:\\Games\\Orca\\Sys");
  EXPECT_EQ(BrandWords("Couldn't open Dolphin.ini"), "Couldn't open Dolphin.ini");
  EXPECT_EQ(BrandWords("Orca.app/Contents/MacOS/Orca"), "Orca.app/Contents/MacOS/Orca");
  EXPECT_EQ(BrandWords("Sys/Orca/RSBE01.ini"), "Sys/Orca/RSBE01.ini");
  EXPECT_EQ(BrandWords("~/.Dolphin"), "~/.Dolphin");
  // A path and a word in one sentence: only the word changes.
  EXPECT_EQ(BrandWords("Dolphin can't write to /tmp/Dolphin/x"),
            "YG Baiji can't write to /tmp/Dolphin/x");
}

TEST(OrcaBranding, TwiceIsOnce)
{
  for (const std::string_view text :
       {"Dolphin", "Orca", "Orca is a fork of Dolphin.", "DolphinNoGUI and Orca.app",
        "Dolphin's Orca's", "YG Baiji", "OrcaDolphin Orca.Dolphin Orca Dolphin."})
  {
    const std::string once = BrandWords(text);
    EXPECT_EQ(BrandWords(once), once) << text;
    EXPECT_EQ(once.find("YG YG"), std::string::npos) << text;
  }
}
