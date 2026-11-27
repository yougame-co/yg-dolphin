// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/TextEntry.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h>
// The undo record's layout (ImGuiInputTextState::Stb), declared as imgui_widgets.cpp declares it.
namespace ImStb
{
#include <imstb_textedit.h>
}

#include "Core/Orca/Status.h"

namespace Orca::UX::TextEntry
{
namespace
{
// Far more than a person types between two frames; a stuck video thread can't grow it forever.
constexpr std::size_t MAX_QUEUED = 256;

std::atomic<bool> s_capturing = false;
std::mutex s_lock;
std::vector<Event> s_events;   // guarded by s_lock
char16_t s_high_surrogate = 0;  // guarded by s_lock
// The ImGui ids of the fields Input drew (video thread). A screen has a handful.
constexpr std::size_t MAX_FIELDS = 16;
std::vector<ImGuiID> s_field_ids;

template <typename T>
void WipeVector(ImVector<T>& vector)
{
  if (vector.Data)
    WipeBytes(vector.Data, static_cast<std::size_t>(vector.Capacity) * sizeof(T));
}

// Zeroes every byte the vector owns, past its size too, then empties it (keeping the buffer).
void WipeEvents(std::vector<Event>& events)
{
  if (events.data())
    WipeBytes(events.data(), events.capacity() * sizeof(Event));
  events.clear();
}

void Push(const Event& event)
{
  if (!s_capturing.load(std::memory_order_relaxed))
    return;
  std::lock_guard lock(s_lock);
  // The whole queue at once, the first time: growing it later would free a buffer of typed
  // characters without wiping it.
  if (s_events.capacity() < MAX_QUEUED)
    s_events.reserve(MAX_QUEUED);
  if (s_events.size() < MAX_QUEUED)
    s_events.push_back(event);
}

bool Printable(char32_t point)
{
  return point >= 0x20 && point != 0x7F && !(point >= 0x80 && point < 0xA0) &&
         !(point >= 0xD800 && point <= 0xDFFF) && !(point >= 0xF700 && point <= 0xF8FF) &&
         point <= 0x10FFFF;
}

void PushPoint(char32_t point)
{
  if (!Printable(point))
    return;
  Event event;
  event.point = point;
  Push(event);
}
}  // namespace

bool Capturing()
{
  return s_capturing.load(std::memory_order_relaxed);
}

void SetCapturing(bool capturing)
{
  // The app forwards the keys typed in its window only between these lines (AppLine).
  if (s_capturing.exchange(capturing, std::memory_order_relaxed) != capturing)
    Status::Line(capturing ? TEXT_ON : TEXT_OFF);
  std::lock_guard lock(s_lock);
  WipeEvents(s_events);
  s_high_surrogate = 0;
}

void Press(Key key, bool shift)
{
  if (key == Key::NoKey)
    return;
  Event event;
  event.key = key;
  event.shift = shift;
  Push(event);
}

void Type(std::string_view utf8)
{
  if (!Capturing())
    return;
  const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(utf8[i]); };
  for (std::size_t pos = 0; pos < utf8.size();)
  {
    const unsigned char lead = byte(pos);
    std::size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 :
                                       (lead & 0xF8) == 0xF0     ? 4 :
                                                                   0;
    if (length == 0 || pos + length > utf8.size())
    {
      ++pos;
      continue;
    }
    char32_t point = length == 1 ? lead : lead & (0x7F >> length);
    bool ok = true;
    for (std::size_t i = 1; i < length && ok; ++i)
    {
      ok = (byte(pos + i) & 0xC0) == 0x80;
      point = (point << 6) | (byte(pos + i) & 0x3F);
    }
    static constexpr char32_t MIN[] = {0, 0, 0x80, 0x800, 0x10000};
    if (ok && point >= MIN[length])
      PushPoint(point);
    pos += ok ? length : 1;
  }
}

void TypeUtf16(unsigned unit)
{
  if (!Capturing())
    return;
  char32_t point = 0;
  {
    std::lock_guard lock(s_lock);
    if (unit >= 0xD800 && unit <= 0xDBFF)
    {
      s_high_surrogate = static_cast<char16_t>(unit);
      return;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF)
    {
      if (s_high_surrogate == 0)
        return;
      point = 0x10000 + ((char32_t(s_high_surrogate) - 0xD800) << 10) + (unit - 0xDC00);
    }
    else
    {
      point = unit;
    }
    s_high_surrogate = 0;
  }
  PushPoint(point);
}

bool AppLine(std::string_view line)
{
  constexpr std::string_view TEXT = "text ";
  constexpr std::string_view KEY = "key ";
  if (line.starts_with(TEXT))
  {
    const std::string_view hex = line.substr(TEXT.size());
    if (!Capturing() || hex.empty() || hex.size() % 2 != 0 || hex.size() / 2 > MAX_APP_TEXT)
      return true;
    const auto nibble = [](char c) {
      return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    char bytes[MAX_APP_TEXT];
    std::size_t n = 0;
    bool ok = true;
    for (std::size_t i = 0; i < hex.size() && ok; i += 2)
    {
      const int high = nibble(hex[i]), low = nibble(hex[i + 1]);
      ok = high >= 0 && low >= 0;
      if (ok)
        bytes[n++] = static_cast<char>((high << 4) | low);
    }
    if (ok)
      Type(std::string_view(bytes, n));
    WipeBytes(bytes, sizeof(bytes));
    return true;
  }
  if (line.starts_with(KEY))
  {
    std::string_view rest = line.substr(KEY.size());
    bool shift = false;
    if (const std::size_t space = rest.find(' '); space != std::string_view::npos)
    {
      if (rest.substr(space) != " shift")
        return true;
      shift = true;
      rest = rest.substr(0, space);
    }
    static constexpr std::pair<std::string_view, Key> NAMES[] = {
        {"tab", Key::Tab},   {"enter", Key::Enter}, {"escape", Key::Escape},
        {"backspace", Key::Backspace}, {"delete", Key::Delete}, {"left", Key::Left},
        {"right", Key::Right}, {"up", Key::Up}, {"down", Key::Down}, {"home", Key::Home},
        {"end", Key::End},
    };
    for (const auto& [name, key] : NAMES)
    {
      if (rest == name)
      {
        Press(key, shift);
        break;
      }
    }
    return true;
  }
  return false;
}

Key KeyFromWin32(unsigned virtual_key)
{
  // Windows' VK_ codes (winuser.h).
  switch (virtual_key)
  {
  case 0x09:
    return Key::Tab;
  case 0x0D:
    return Key::Enter;
  case 0x1B:
    return Key::Escape;
  case 0x08:
    return Key::Backspace;
  case 0x2E:
    return Key::Delete;
  case 0x25:
    return Key::Left;
  case 0x26:
    return Key::Up;
  case 0x27:
    return Key::Right;
  case 0x28:
    return Key::Down;
  case 0x24:
    return Key::Home;
  case 0x23:
    return Key::End;
  default:
    return Key::NoKey;
  }
}

Key KeyFromMac(unsigned key_code)
{
  // Carbon's kVK_ codes (HIToolbox/Events.h).
  switch (key_code)
  {
  case 0x30:
    return Key::Tab;
  case 0x24:  // Return
  case 0x4C:  // keypad Enter
    return Key::Enter;
  case 0x35:
    return Key::Escape;
  case 0x33:  // Delete, the backspace key
    return Key::Backspace;
  case 0x75:  // ForwardDelete
    return Key::Delete;
  case 0x7B:
    return Key::Left;
  case 0x7C:
    return Key::Right;
  case 0x7D:
    return Key::Down;
  case 0x7E:
    return Key::Up;
  case 0x73:
    return Key::Home;
  case 0x77:
    return Key::End;
  default:
    return Key::NoKey;
  }
}

Key KeyFromX11(unsigned long keysym)
{
  // X11's XK_ keysyms (X11/keysymdef.h).
  switch (keysym)
  {
  case 0xFF09:  // Tab
  case 0xFE20:  // ISO_Left_Tab, Shift+Tab
    return Key::Tab;
  case 0xFF0D:  // Return
  case 0xFF8D:  // KP_Enter
    return Key::Enter;
  case 0xFF1B:
    return Key::Escape;
  case 0xFF08:
    return Key::Backspace;
  case 0xFFFF:
    return Key::Delete;
  case 0xFF51:
    return Key::Left;
  case 0xFF52:
    return Key::Up;
  case 0xFF53:
    return Key::Right;
  case 0xFF54:
    return Key::Down;
  case 0xFF50:
    return Key::Home;
  case 0xFF57:
    return Key::End;
  default:
    return Key::NoKey;
  }
}

Drained::~Drained()
{
  WipeEvents(m_events);
}

Drained Drain()
{
  Drained out;
  std::lock_guard lock(s_lock);
  if (s_events.empty())
    return out;
  // Sized once, so the copy never grows into a second buffer.
  out.m_events.reserve(s_events.size());
  out.m_events.assign(s_events.begin(), s_events.end());
  WipeEvents(s_events);
  return out;
}

void FeedImGui(const Event& event)
{
  ImGuiIO& io = ImGui::GetIO();
  if (event.key == Key::NoKey)
  {
    io.AddInputCharacter(static_cast<unsigned int>(event.point));
    return;
  }
  ImGuiKey key = ImGuiKey_None;
  switch (event.key)
  {
  case Key::Backspace:
    key = ImGuiKey_Backspace;
    break;
  case Key::Delete:
    key = ImGuiKey_Delete;
    break;
  case Key::Left:
    key = ImGuiKey_LeftArrow;
    break;
  case Key::Right:
    key = ImGuiKey_RightArrow;
    break;
  case Key::Home:
    key = ImGuiKey_Home;
    break;
  case Key::End:
    key = ImGuiKey_End;
    break;
  default:
    return;
  }
  // A press: down and up together. ImGui's input queue trickles them over two frames, so the
  // field sees one press and never a held key that would auto-repeat.
  if (event.shift)
    io.AddKeyEvent(ImGuiMod_Shift, true);
  io.AddKeyEvent(key, true);
  io.AddKeyEvent(key, false);
  if (event.shift)
    io.AddKeyEvent(ImGuiMod_Shift, false);
}

bool Input(const char* id, char* text, std::size_t size, float x, float y, float width,
           float height, bool password, bool focus)
{
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  const float pad_y = std::max(0.0f, (height - ImGui::GetFontSize()) / 2);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(height * 0.3f, pad_y));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  // The screen draws the box; ImGui draws the text and the cursor.
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
  ImGui::PushItemWidth(width);
  if (focus)
    ImGui::SetKeyboardFocusHere();
  ImGui::InputText(id, text, size, password ? ImGuiInputTextFlags_Password : 0);
  const ImGuiID item = ImGui::GetItemID();
  if (item != 0 && s_field_ids.size() < MAX_FIELDS &&
      std::find(s_field_ids.begin(), s_field_ids.end(), item) == s_field_ids.end())
  {
    s_field_ids.push_back(item);
  }
  const bool active = ImGui::IsItemActive();
  ImGui::PopItemWidth();
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(2);
  return active;
}

void ForgetImGuiText()
{
  ImGuiContext* context = ImGui::GetCurrentContext();
  if (!context)
    return;
  ImGuiContext& g = *context;
  const auto ours = [](ImGuiID id) {
    return id != 0 && std::find(s_field_ids.begin(), s_field_ids.end(), id) != s_field_ids.end();
  };
  // Ending the edit first: ImGui then copies the text into InputTextDeactivatedState, wiped below.
  if (ours(g.ActiveId))
    ImGui::ClearActiveID();
  ImGuiInputTextState& state = g.InputTextState;
  if (ours(state.ID))
  {
    WipeVector(state.TextA);
    WipeVector(state.TextToRevertTo);
    WipeVector(state.CallbackTextBackup);
    if (state.Stb)
    {
      // Deleted text is kept for undo even in a password field.
      ImStb::StbUndoState& undo = state.Stb->undostate;
      WipeBytes(undo.undo_rec, sizeof(undo.undo_rec));
      WipeBytes(undo.undo_char, sizeof(undo.undo_char));
      undo.undo_point = 0;
      undo.undo_char_point = 0;
      undo.redo_point = IMSTB_TEXTEDIT_UNDOSTATECOUNT;
      undo.redo_char_point = IMSTB_TEXTEDIT_UNDOCHARCOUNT;
      state.Stb->cursor = 0;
      state.Stb->select_start = 0;
      state.Stb->select_end = 0;
    }
    state.TextLen = 0;
    // No widget owns it now: the next edit starts from the field's own buffer.
    state.ID = 0;
  }
  ImGuiInputTextDeactivatedState& deactivated = g.InputTextDeactivatedState;
  if (ours(deactivated.ID))
  {
    WipeVector(deactivated.TextA);
    deactivated.ClearFreeMemory();
  }
  // Characters fed but not yet used, and this frame's record of the ones that were.
  WipeVector(g.IO.InputQueueCharacters);
  g.IO.InputQueueCharacters.resize(0);
  for (ImGuiInputEvent& event : g.InputEventsQueue)
  {
    if (event.Type == ImGuiInputEventType_Text)
      event.Text.Char = 0;
  }
  for (int i = 0; i < g.InputEventsQueue.Size;)
  {
    if (g.InputEventsQueue[i].Type == ImGuiInputEventType_Text)
      g.InputEventsQueue.erase(g.InputEventsQueue.Data + i);
    else
      ++i;
  }
  for (ImGuiInputEvent& event : g.InputEventsTrail)
  {
    if (event.Type == ImGuiInputEventType_Text)
      event.Text.Char = 0;
  }
}
}  // namespace Orca::UX::TextEntry
