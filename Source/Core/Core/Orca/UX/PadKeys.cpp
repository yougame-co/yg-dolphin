// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/PadKeys.h"

namespace Orca::UX
{
using TextEntry::Key;

u8 PadKeys::StickDirections(const GCPadStatus& pad) const
{
  const int x = static_cast<int>(pad.stickX) - GCPadStatus::MAIN_STICK_CENTER_X;
  const int y = static_cast<int>(pad.stickY) - GCPadStatus::MAIN_STICK_CENTER_Y;
  // A direction already held stays held until the stick comes back near the centre.
  const auto held = [&](int value, u8 direction) {
    return value >= ((m_stick & direction) ? STICK_OFF : STICK_ON);
  };
  u8 directions = 0;
  if (held(y, UP))
    directions |= UP;
  if (held(-y, DOWN))
    directions |= DOWN;
  if (held(-x, LEFT))
    directions |= LEFT;
  if (held(x, RIGHT))
    directions |= RIGHT;
  return directions;
}

PadKeys::Step PadKeys::Next(const GCPadStatus& pad, bool capturing)
{
  Step step;
  const u16 buttons = pad.button & BUTTONS;
  const u8 stick = StickDirections(pad);
  if (capturing)
  {
    // The first sample after the screen opened only sets what counts as already down.
    if (m_capturing)
    {
      const u16 pressed = buttons & static_cast<u16>(~m_buttons);
      const u8 pushed = stick & static_cast<u8>(~m_stick);
      if (pressed & PAD_BUTTON_B)
        step.keys.push_back(Key::Escape);
      if (pressed & PAD_BUTTON_A)
        step.keys.push_back(Key::Enter);
      if ((pressed & PAD_BUTTON_UP) || (pushed & UP))
        step.keys.push_back(Key::Up);
      if ((pressed & PAD_BUTTON_DOWN) || (pushed & DOWN))
        step.keys.push_back(Key::Down);
      if ((pressed & PAD_BUTTON_LEFT) || (pushed & LEFT))
        step.keys.push_back(Key::Left);
      if ((pressed & PAD_BUTTON_RIGHT) || (pushed & RIGHT))
        step.keys.push_back(Key::Right);
      m_pressed |= pressed;
    }
    else
    {
      m_pressed = 0;
    }
    m_pressed &= buttons;
    m_hidden = 0;
  }
  else
  {
    // Just closed: what was pressed on the screen and is still down stays hidden until let go.
    if (m_capturing)
      m_hidden = m_pressed;
    m_pressed = 0;
    m_hidden &= buttons;
    step.hidden = m_hidden;
  }
  m_capturing = capturing;
  m_buttons = buttons;
  m_stick = stick;
  return step;
}
}  // namespace Orca::UX
