// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

// The YouGame button's shortcut on a controller (Luis, 2026-10-05): hold the D-pad's Up and press
// Start, and the YouGame overlay opens, as a click on the button Orca draws (YgOrb.h) does: Orca
// prints `orca orb` and the page opens it. A GameCube pad has no Home button, and this works the
// same on every controller and the keyboard, since it reads the local player's pad as the game will
// get it, in every scene.
//
// While the shortcut is held, the game gets neither Start nor the D-pad (no pause, no second taunt),
// from the press of Start until both are let go; Up before Start reaches the game as usual (a taunt
// in a fight). The session records and sends the masked pad, so every machine plays the same input
// and re-runs replay it: nothing here can desync.
namespace Orca::UX
{
class OrbCombo
{
public:
  static constexpr u16 START = PAD_BUTTON_START;
  static constexpr u16 UP = PAD_BUTTON_UP;
  static constexpr u16 DPAD = PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT;

  struct Step
  {
    u16 buttons = 0;  // what the game gets
    bool fired = false;
  };
  // One sample of the local player's buttons, a frame's own (a first run). Pure: no I/O.
  Step Next(u16 buttons);
  bool Held() const { return m_held; }

private:
  bool m_held = false;   // fired: Start and the D-pad masked until both are let go
  bool m_start = false;  // Start was down in the last sample
};

// The local player's pad as the session will send it: the shortcut stepped on every read a session
// can send (`first_run`: not a rollback's re-run, whose read is thrown away; a joiner's catch-up
// does send) while `live` (the input gate is open: an unfocused pad is neutral and must not look
// like a release), and `orca orb` printed when it fires. CPU thread; no allocation.
GCPadStatus FilterLocalPad(GCPadStatus pad, bool first_run, bool live);
}  // namespace Orca::UX
