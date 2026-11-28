// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/UX/PadKeys.h"
#include "Core/Orca/UX/TextEntry.h"

using Orca::UX::PadKeys;
namespace TE = Orca::UX::TextEntry;
using Keys = std::vector<TE::Key>;

namespace
{
GCPadStatus Pad(u16 buttons = 0, int stick_x = 0, int stick_y = 0)
{
  GCPadStatus pad;
  pad.button = buttons;
  pad.stickX = static_cast<u8>(GCPadStatus::MAIN_STICK_CENTER_X + stick_x);
  pad.stickY = static_cast<u8>(GCPadStatus::MAIN_STICK_CENTER_Y + stick_y);
  return pad;
}

// A PadKeys whose screen has been open for one sample with nothing down.
PadKeys Open()
{
  PadKeys keys;
  keys.Next(Pad(), false);
  keys.Next(Pad(), true);
  return keys;
}
}  // namespace

TEST(OrcaPadKeys, NothingWhileNoScreenIsOpen)
{
  PadKeys keys;
  for (const u16 buttons : {u16(0), u16(PAD_BUTTON_B), u16(0), u16(PAD_BUTTON_A)})
  {
    const PadKeys::Step step = keys.Next(Pad(buttons), false);
    EXPECT_TRUE(step.keys.empty());
    EXPECT_EQ(step.hidden, 0);
  }
}

TEST(OrcaPadKeys, ButtonsBecomeTheScreensKeys)
{
  PadKeys keys = Open();
  EXPECT_EQ(keys.Next(Pad(PAD_BUTTON_B), true).keys, Keys{TE::Key::Escape});
  EXPECT_TRUE(keys.Next(Pad(PAD_BUTTON_B), true).keys.empty());  // held: once
  EXPECT_TRUE(keys.Next(Pad(), true).keys.empty());
  EXPECT_EQ(keys.Next(Pad(PAD_BUTTON_A), true).keys, Keys{TE::Key::Enter});
  EXPECT_EQ(keys.Next(Pad(PAD_BUTTON_UP), true).keys, Keys{TE::Key::Up});
  EXPECT_EQ(keys.Next(Pad(PAD_BUTTON_LEFT), true).keys, Keys{TE::Key::Left});
  EXPECT_EQ(keys.Next(Pad(PAD_BUTTON_DOWN), true).keys, Keys{TE::Key::Down});
  EXPECT_EQ(keys.Next(Pad(PAD_BUTTON_RIGHT), true).keys, Keys{TE::Key::Right});
  // Buttons the screen has no use for do nothing.
  EXPECT_TRUE(keys.Next(Pad(PAD_BUTTON_X | PAD_BUTTON_START), true).keys.empty());
}

TEST(OrcaPadKeys, ButtonsDownWhenItOpensWaitForARelease)
{
  PadKeys keys;
  keys.Next(Pad(PAD_BUTTON_A | PAD_BUTTON_B), false);
  // The A that opened the page, the B held through it: neither presses anything.
  EXPECT_TRUE(keys.Next(Pad(PAD_BUTTON_A | PAD_BUTTON_B), true).keys.empty());
  EXPECT_TRUE(keys.Next(Pad(PAD_BUTTON_B), true).keys.empty());
  EXPECT_TRUE(keys.Next(Pad(), true).keys.empty());
  EXPECT_EQ(keys.Next(Pad(PAD_BUTTON_B), true).keys, Keys{TE::Key::Escape});
}

TEST(OrcaPadKeys, AButtonPressedAsItOpensAlsoWaits)
{
  // The first sample after it opens only sets what counts as down, even after a gap in samples.
  PadKeys keys;
  EXPECT_TRUE(keys.Next(Pad(PAD_BUTTON_A), true).keys.empty());
  EXPECT_TRUE(keys.Next(Pad(PAD_BUTTON_A), true).keys.empty());
}

TEST(OrcaPadKeys, TheStickMovesOncePerPush)
{
  PadKeys keys = Open();
  EXPECT_TRUE(keys.Next(Pad(0, 0, PadKeys::STICK_ON - 1), true).keys.empty());
  EXPECT_EQ(keys.Next(Pad(0, 0, PadKeys::STICK_ON), true).keys, Keys{TE::Key::Up});
  EXPECT_TRUE(keys.Next(Pad(0, 0, 100), true).keys.empty());
  // Back a little, not to the centre: still the same push.
  EXPECT_TRUE(keys.Next(Pad(0, 0, PadKeys::STICK_OFF), true).keys.empty());
  EXPECT_TRUE(keys.Next(Pad(0, 0, PadKeys::STICK_ON), true).keys.empty());
  EXPECT_TRUE(keys.Next(Pad(0, 0, PadKeys::STICK_OFF - 1), true).keys.empty());
  EXPECT_EQ(keys.Next(Pad(0, 0, PadKeys::STICK_ON), true).keys, Keys{TE::Key::Up});
  EXPECT_TRUE(keys.Next(Pad(), true).keys.empty());
  EXPECT_EQ(keys.Next(Pad(0, 0, -PadKeys::STICK_ON), true).keys, Keys{TE::Key::Down});
  EXPECT_TRUE(keys.Next(Pad(), true).keys.empty());
  EXPECT_EQ(keys.Next(Pad(0, -PadKeys::STICK_ON, 0), true).keys, Keys{TE::Key::Left});
  EXPECT_TRUE(keys.Next(Pad(), true).keys.empty());
  EXPECT_EQ(keys.Next(Pad(0, PadKeys::STICK_ON, 0), true).keys, Keys{TE::Key::Right});
}

TEST(OrcaPadKeys, AStickPushedAsItOpensWaits)
{
  PadKeys keys;
  keys.Next(Pad(0, 0, 100), false);
  EXPECT_TRUE(keys.Next(Pad(0, 0, 100), true).keys.empty());
  EXPECT_TRUE(keys.Next(Pad(0, 0, 100), true).keys.empty());
  EXPECT_TRUE(keys.Next(Pad(), true).keys.empty());
  EXPECT_EQ(keys.Next(Pad(0, 0, 100), true).keys, Keys{TE::Key::Up});
}

TEST(OrcaPadKeys, TheButtonThatClosedItIsHiddenUntilLetGo)
{
  PadKeys keys = Open();
  EXPECT_EQ(keys.Next(Pad(PAD_BUTTON_B), true).keys, Keys{TE::Key::Escape});
  // The screen closed under the held B: the game must not see it as a fresh press.
  PadKeys::Step step = keys.Next(Pad(PAD_BUTTON_B), false);
  EXPECT_EQ(step.hidden, PAD_BUTTON_B);
  EXPECT_EQ(keys.Hidden(), PAD_BUTTON_B);
  // A button pressed after it closed is the game's.
  step = keys.Next(Pad(PAD_BUTTON_B | PAD_BUTTON_A), false);
  EXPECT_EQ(step.hidden, PAD_BUTTON_B);
  step = keys.Next(Pad(PAD_BUTTON_A), false);
  EXPECT_EQ(step.hidden, 0);
  // Let go once: the next B is the game's.
  step = keys.Next(Pad(PAD_BUTTON_B), false);
  EXPECT_EQ(step.hidden, 0);
  EXPECT_TRUE(step.keys.empty());
}

TEST(OrcaPadKeys, OnlyTheScreensButtonsAreHidden)
{
  PadKeys keys = Open();
  keys.Next(Pad(PAD_BUTTON_A | PAD_BUTTON_X | PAD_BUTTON_UP), true);
  EXPECT_EQ(keys.Next(Pad(PAD_BUTTON_A | PAD_BUTTON_X | PAD_BUTTON_UP), false).hidden,
            PAD_BUTTON_A | PAD_BUTTON_UP);
}

TEST(OrcaPadKeys, ButtonsHeldFromBeforeItOpenedAreNotHidden)
{
  PadKeys keys;
  keys.Next(Pad(PAD_BUTTON_A), false);
  EXPECT_TRUE(keys.Next(Pad(PAD_BUTTON_A), true).keys.empty());
  EXPECT_EQ(keys.Next(Pad(PAD_BUTTON_A | PAD_BUTTON_B), true).keys, Keys{TE::Key::Escape});
  // The A was down all along; only the B was pressed on the screen.
  EXPECT_EQ(keys.Next(Pad(PAD_BUTTON_A | PAD_BUTTON_B), false).hidden, PAD_BUTTON_B);
  EXPECT_EQ(keys.Hidden(), PAD_BUTTON_B);
}

TEST(OrcaPadKeys, APressLetGoOnTheScreenIsNotHidden)
{
  PadKeys keys = Open();
  EXPECT_EQ(keys.Next(Pad(PAD_BUTTON_A), true).keys, Keys{TE::Key::Enter});
  keys.Next(Pad(), true);
  // Let go on the screen, then pressed as it closes: that press is the game's.
  EXPECT_EQ(keys.Next(Pad(PAD_BUTTON_A), false).hidden, 0);
}

TEST(OrcaPadKeys, ReopeningClearsWhatWasHidden)
{
  PadKeys keys = Open();
  keys.Next(Pad(PAD_BUTTON_B), true);
  EXPECT_EQ(keys.Next(Pad(PAD_BUTTON_B), false).hidden, PAD_BUTTON_B);
  EXPECT_EQ(keys.Next(Pad(PAD_BUTTON_B), true).hidden, 0);
  EXPECT_EQ(keys.Hidden(), 0);
}

TEST(OrcaPadKeys, ThePadsKeysReachTheScreen)
{
  TE::SetCapturing(true);
  PadKeys keys;
  keys.Next(Pad(), TE::Capturing());
  for (const u16 buttons : {u16(PAD_BUTTON_DOWN), u16(0), u16(PAD_BUTTON_A), u16(0),
                            u16(PAD_BUTTON_B)})
  {
    for (const TE::Key key : keys.Next(Pad(buttons), TE::Capturing()).keys)
      TE::PressFromPad(key);
  }
  Keys drained;
  for (const TE::Event& event : TE::Drain())
    drained.push_back(event.key);
  EXPECT_EQ(drained, (Keys{TE::Key::Down, TE::Key::Enter, TE::Key::Escape}));
  TE::SetCapturing(false);
  // Closed: a press queues nothing.
  for (const TE::Key key : keys.Next(Pad(PAD_BUTTON_A), TE::Capturing()).keys)
    TE::Press(key, false);
  TE::SetCapturing(true);
  EXPECT_TRUE(TE::Drain().empty());
  TE::SetCapturing(false);
}
