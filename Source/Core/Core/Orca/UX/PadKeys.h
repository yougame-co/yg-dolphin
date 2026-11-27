// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/TextEntry.h"
#include "InputCommon/GCPadStatus.h"

// The local pad on a screen that holds the keyboard (UX/TextEntry.h, UX/BrawlOnlineSignIn.h).
// Under the desktop app the keyboard goes to the app's window, not Orca's, so without this a player
// on a pad could not leave the screen: every pad reads neutral while it is open.
//
// A press while the screen is open becomes one of the screen's keys: A is Enter, B is Escape, and
// the D-pad or the main stick moves the focus (Up and Left to the previous item, Down and Right to
// the next). Only presses made while it is open count: buttons already down when it opens wait for
// a release first. A button pressed while it is open and still down when it closes stays hidden from
// the game until it is let go, so the B that closed it does not also take the game back a page.
// Buttons held from before it opened are left alone.
//
// Host-only: the screen never opens during online play (BrawlOnlineSignIn::OnlineActive), and what
// the game gets is the pad the session records, as always.
namespace Orca::UX
{
class PadKeys
{
public:
  static constexpr u16 BUTTONS = PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_UP | PAD_BUTTON_DOWN |
                                 PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT;
  // How far from centre the main stick counts as pushed, and how near it must come back first.
  static constexpr int STICK_ON = 56;
  static constexpr int STICK_OFF = 28;

  struct Step
  {
    std::vector<TextEntry::Key> keys;  // pressed this sample, oldest first
    u16 hidden = 0;  // buttons the game must not see this sample
  };
  // One sample of the local pad, and whether a screen holds the keyboard. Pure, no I/O.
  Step Next(const GCPadStatus& pad, bool capturing);
  // The buttons hidden from the game since the screen closed, without taking a sample.
  u16 Hidden() const { return m_hidden; }

private:
  enum Direction : u8
  {
    UP = 1,
    DOWN = 2,
    LEFT = 4,
    RIGHT = 8,
  };
  u8 StickDirections(const GCPadStatus& pad) const;

  bool m_capturing = false;
  u16 m_buttons = 0;  // the last sample's
  u8 m_stick = 0;     // directions the stick held at the last sample
  u16 m_pressed = 0;  // buttons pressed while the screen is open and not let go since
  u16 m_hidden = 0;
};
}  // namespace Orca::UX
