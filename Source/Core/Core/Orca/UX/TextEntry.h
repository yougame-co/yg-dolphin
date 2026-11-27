// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

// Typing into Orca's own overlay screens (UX/BrawlOnlineSignIn.h). DolphinNoGUI feeds ImGui no
// keyboard, so while a screen captures the keyboard each platform's window loop hands its key
// presses and typed text here instead of to the game, the shortcuts and Orca's own keys (Escape,
// F8, Shift+Tab). The screen drains them on the video thread and passes them to ImGui.
//
// The game never sees them: while Capturing(), every pad reads neutral, whatever background input
// or ORCA_VIRTUAL_PAD_ONLY say. Core::UpdateInputGate closes the input gate for every Dolphin device,
// and the session's pad reads (the app's, UX/ControllerSource.h LocalPad, and Dolphin's own in
// Rollback's local pad) check Capturing() themselves, so the first frame is neutral too, before the
// gate next moves. The app's pads reach the screen as keys instead (UX/PadKeys.h, through Press).
// Host-only: nothing here reads or writes emulated state.
namespace Orca::UX::TextEntry
{
// The keys a screen acts on. Everything else typed arrives as text.
enum class Key
{
  NoKey,  // not one of these (X11 defines None)
  Tab,
  Enter,
  Escape,
  Backspace,
  Delete,
  Left,
  Right,
  Up,
  Down,
  Home,
  End,
};

struct Event
{
  Key key = Key::NoKey;  // NoKey: a text event
  bool shift = false;
  char32_t point = 0;  // the typed character, for a text event
};

// Whether a screen holds the keyboard. Any thread; one atomic load.
bool Capturing();
// Whether a window loop hands a key event to the screen instead of the game, its shortcuts and
// Orca's keys: a press or typed text, only while `capturing`. A release always passes, so nothing
// stays held. Every platform loop decides through this.
constexpr bool Swallows(bool capturing, bool release)
{
  return capturing && !release;
}
// The screen opened or closed. Closing drops anything not yet drained.
void SetCapturing(bool capturing);

// Window loop: a key went down (repeats included; key-ups are not needed). The loop sends no text
// for a press with Ctrl, or Cmd on macOS, held. Ignored unless Capturing().
void Press(Key key, bool shift);
// Window loop: typed text, UTF-8. Control characters, invalid UTF-8 and macOS's function-key
// characters (U+F700-U+F8FF) are dropped. Ignored unless Capturing().
void Type(std::string_view utf8);
// Windows' WM_CHAR, one UTF-16 unit at a time (a surrogate pair arrives as two).
void TypeUtf16(unsigned unit);

// The keys each platform's window loop reports, from its own key codes.
Key KeyFromWin32(unsigned virtual_key);
Key KeyFromMac(unsigned key_code);
Key KeyFromX11(unsigned long keysym);

// What Drain hands over. Typed characters are as secret as the password they spell, so the batch
// zeroes its whole buffer when it goes out of scope, on every path out of the loop that reads it.
class Drained
{
public:
  Drained() = default;
  Drained(Drained&& other) noexcept : m_events(std::move(other.m_events)) {}
  Drained(const Drained&) = delete;
  Drained& operator=(const Drained&) = delete;
  Drained& operator=(Drained&&) = delete;
  ~Drained();

  std::vector<Event>::const_iterator begin() const { return m_events.begin(); }
  std::vector<Event>::const_iterator end() const { return m_events.end(); }
  bool empty() const { return m_events.empty(); }
  std::size_t size() const { return m_events.size(); }
  const Event& operator[](std::size_t i) const { return m_events[i]; }

private:
  friend Drained Drain();
  std::vector<Event> m_events;
};

// Video thread: everything pressed and typed since the last call, oldest first.
Drained Drain();

// Video thread, inside an ImGui frame: passes an editing key or a character to ImGui's focused
// text field. Tab, Enter, Escape, Up and Down are the screen's own and are not passed.
void FeedImGui(const Event& event);

// Overwrites `size` bytes at `data` with zeros through a volatile pointer, so the compiler keeps the
// stores even when the memory is about to be freed. Plain C++: the same on every OS.
inline void WipeBytes(void* data, std::size_t size)
{
  volatile unsigned char* bytes = static_cast<volatile unsigned char*>(data);
  for (std::size_t i = 0; i < size; ++i)
    bytes[i] = 0;
}

// One line of text in a fixed buffer: ImGui InputText edits it in place, and Wipe zeroes it.
template <std::size_t N>
struct Field
{
  static_assert(N > 1);
  char text[N] = {};
  std::string_view View() const { return {text, Length()}; }
  std::size_t Length() const
  {
    std::size_t n = 0;
    while (n < N && text[n] != '\0')
      ++n;
    return n;
  }
  void Set(std::string_view value)
  {
    Wipe();
    const std::size_t n = value.size() < N - 1 ? value.size() : N - 1;
    for (std::size_t i = 0; i < n; ++i)
      text[i] = value[i];
  }
  void Wipe() { WipeBytes(text, N); }
};

// Video thread, inside an ImGui window: an ImGui InputText over [x, x + width] at `y`, `height`
// tall. `password` masks it. `focus` asks for the keyboard this frame (pass it on the frame the
// field becomes the focused one). Returns whether ImGui holds the field active afterwards.
bool Input(const char* id, char* text, std::size_t size, float x, float y, float width,
           float height, bool password, bool focus);

// Video thread, with an ImGui context: zeroes ImGui's own copies of what was typed into the fields
// Input drew (the active edit buffer, its revert copy, the undo record, the last deactivated
// field's text, and typed characters ImGui has queued but not used) and ends any edit in them, so
// the next frame reads the fields' own buffers again.
void ForgetImGuiText();
}  // namespace Orca::UX::TextEntry
