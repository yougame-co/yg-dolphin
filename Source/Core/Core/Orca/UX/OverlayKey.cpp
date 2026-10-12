// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/OverlayKey.h"

namespace Orca::UX::OverlayKey
{
namespace
{
struct Lines
{
  std::string_view down, up, cancel;
};
constexpr Lines KEYS{"orca overlay down keys", "orca overlay up keys", "orca overlay cancel keys"};
constexpr Lines PAD{"orca overlay down pad", "orca overlay up pad", "orca overlay cancel pad"};
}  // namespace

std::string_view Press::Down(bool talk)
{
  std::lock_guard lock(m_lock);
  if (m_down)
    return {};
  if (!talk)
    return TOGGLE;
  m_down = true;
  return (m_source == Source::Keys ? KEYS : PAD).down;
}

std::string_view Press::Up()
{
  std::lock_guard lock(m_lock);
  if (!m_down)
    return {};
  m_down = false;
  return (m_source == Source::Keys ? KEYS : PAD).up;
}

std::string_view Press::Cancel()
{
  std::lock_guard lock(m_lock);
  if (!m_down)
    return {};
  m_down = false;
  return (m_source == Source::Keys ? KEYS : PAD).cancel;
}

bool Press::IsDown() const
{
  std::lock_guard lock(m_lock);
  return m_down;
}

Press& Keys()
{
  static Press s_keys(Source::Keys);
  return s_keys;
}

Press& Pad()
{
  static Press s_pad(Source::Pad);
  return s_pad;
}
}  // namespace Orca::UX::OverlayKey
