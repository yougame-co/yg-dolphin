// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <string_view>

// Press and release of YouGame's overlay key, for hold-to-talk (cap "talk"). Orca reports the two
// presses only it can see: Shift+Tab while its own window has the keyboard (Windows), and the
// controller shortcut (UX/OrbCombo.h). With "talk" agreed a press prints
//   orca overlay down keys|pad
// and then exactly one of
//   orca overlay up keys|pad       let go
//   orca overlay cancel keys|pad   no longer read (focus lost, window hidden, game paused)
// The page tells a tap (its menu) from a hold (talk) by the time between them. Without "talk" a
// press prints "orca orb", a toggle, and nothing follows. Host-only: nothing here touches emulated
// state.
namespace Orca::UX::OverlayKey
{
inline constexpr std::string_view TOGGLE = "orca orb";

enum class Source
{
  Keys,
  Pad,
};

// One source's press. Each call returns the line to print, or an empty view. Thread-safe.
class Press
{
public:
  explicit Press(Source source) : m_source(source) {}
  // Pressed (never a key repeat). `talk`: the app agreed "talk". Nothing while already down.
  std::string_view Down(bool talk);
  // Let go: "up", once per "down".
  std::string_view Up();
  // No longer read: "cancel", once per "down".
  std::string_view Cancel();
  bool IsDown() const;

private:
  mutable std::mutex m_lock;
  const Source m_source;
  bool m_down = false;
};

// The process-wide presses.
Press& Keys();
Press& Pad();
}  // namespace Orca::UX::OverlayKey
