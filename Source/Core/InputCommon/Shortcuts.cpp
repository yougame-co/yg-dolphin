// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later
#include "InputCommon/Shortcuts.h"

#include <array>

namespace InputCommon::Shortcuts
{
namespace
{
bool IsKey(std::string_view key)
{
  if (key == "Tab" || key == "Enter" || key == "Backquote")
    return true;
  if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z')
    return true;
  constexpr std::array function_keys = {"F1", "F2", "F3", "F4",  "F5",  "F6",
                                        "F7", "F8", "F9", "F10", "F11", "F12"};
  for (const auto* function_key : function_keys)
    if (key == function_key)
      return true;
  return false;
}
std::optional<Combo> ParseCombo(std::string_view text)
{
  Combo combo;
  while (true)
  {
    const auto plus = text.find('+');
    if (plus == std::string_view::npos)
      break;
    const auto modifier = text.substr(0, plus);
    const unsigned bit = modifier == "Shift" ? Shift :
                         modifier == "Ctrl"  ? Ctrl :
                         modifier == "Alt"   ? Alt :
                                               0;
    if (!bit || (combo.modifiers & bit))
      return std::nullopt;
    combo.modifiers |= bit;
    text.remove_prefix(plus + 1);
  }
  if (!IsKey(text))
    return std::nullopt;
  combo.key = text;
  return combo;
}
}  // namespace
std::optional<Bindings> ParseLine(std::string_view line)
{
  constexpr std::string_view prefix = "embed shortcuts";
  if (!line.starts_with(prefix))
    return std::nullopt;
  line.remove_prefix(prefix.size());
  Bindings result;
  while (!line.empty())
  {
    if (line.front() != ' ')
      return std::nullopt;
    line.remove_prefix(1);
    const auto space = line.find(' ');
    const auto token = line.substr(0, space);
    auto* destination = token.starts_with("menu=") ? &result.menu :
                        token.starts_with("chat=") ? &result.chat :
                                                     nullptr;
    if (!destination || *destination)
      return std::nullopt;
    const auto combo = ParseCombo(token.substr(5));
    if (!combo)
      return std::nullopt;
    *destination = *combo;
    if (space == std::string_view::npos)
      break;
    line.remove_prefix(space);
  }
  return result;
}
void Manager::SetOutput(Output output)
{
  std::lock_guard lock(m_mutex);
  m_output = std::move(output);
}
void Manager::Emit(bool chat, std::string_view edge)
{
  if (m_output)
  {
    const std::string line =
        std::string("yg {\"") + (chat ? "chat" : "overlay") + "\":\"" + std::string(edge) + "\"}";
    m_output(line);
  }
}
void Manager::End(bool chat, bool cancel)
{
  auto& down = chat ? m_chat_down : m_menu_down;
  if (down)
  {
    down = false;
    Emit(chat, chat && cancel ? "cancel" : "up");
  }
}
void Manager::SwallowHeldBindings()
{
  // Suppression is independent of relay presses: a base key held before its modifiers,
  // or a modifier released and pressed again after cancel, must never reach the game.
  for (const auto& combo : {m_bindings.menu, m_bindings.chat})
  {
    if (combo && m_pressed.contains(combo->key) && m_modifiers == combo->modifiers)
    {
      m_suppressed.insert(combo->key);
      if (combo->modifiers & Shift)
        m_suppressed.insert("Shift");
      if (combo->modifiers & Ctrl)
        m_suppressed.insert("Ctrl");
      if (combo->modifiers & Alt)
        m_suppressed.insert("Alt");
    }
  }
}
bool Manager::Configure(std::string_view line)
{
  const auto bindings = ParseLine(line);
  if (!bindings)
    return false;
  std::lock_guard lock(m_mutex);
  if (m_bindings != *bindings)
  {
    End(false, true);
    End(true, true);
    m_bindings = *bindings;
    // Swallow a newly bound chord already held, without a synthetic press.
    SwallowHeldBindings();
  }
  m_configured = true;
  return true;
}
bool Manager::Configured() const
{
  std::lock_guard lock(m_mutex);
  return m_configured;
}
bool Manager::KeyEvent(std::string_view key, unsigned modifiers, bool down, bool repeat)
{
  std::lock_guard lock(m_mutex);
  if (!m_focused)
    return false;
  bool consumed = m_suppressed.contains(key);
  const bool fresh_press = down && !repeat && !key.empty() && !m_pressed.contains(key);
  m_modifiers = modifiers;
  // Unknown platform keys carry modifier updates but have no canonical key state.
  // Repeats may reveal a dropped modifier, but must never begin a new press.
  if (!key.empty() && !repeat)
  {
    if (down)
      m_pressed.insert(std::string(key));
    else
      m_pressed.erase(std::string(key));
  }
  for (const auto& [name, bit] : std::array<std::pair<const char*, unsigned>, 3>{
           {{"Shift", Shift}, {"Ctrl", Ctrl}, {"Alt", Alt}}})
  {
    if (modifiers & bit)
      m_pressed.insert(name);
    else
      m_pressed.erase(name);
  }
  SwallowHeldBindings();
  consumed = consumed || m_suppressed.contains(key);
  // A base-key release is a normal up; losing any required modifier drops the hold.
  const auto update = [&](bool chat, const std::optional<Combo>& combo) {
    if (!combo)
      return;
    auto& held = chat ? m_chat_down : m_menu_down;
    if (held && !down && !repeat && key == combo->key)
      End(chat, false);
    else if (held && modifiers != combo->modifiers)
      End(chat, true);
    if (fresh_press && key == combo->key && modifiers == combo->modifiers && !held)
    {
      held = true;
      m_suppressed.insert(combo->key);
      if (modifiers & Shift)
        m_suppressed.insert("Shift");
      if (modifiers & Ctrl)
        m_suppressed.insert("Ctrl");
      if (modifiers & Alt)
        m_suppressed.insert("Alt");
      consumed = true;
      Emit(chat, "down");
    }
  };
  update(false, m_bindings.menu);
  update(true, m_bindings.chat);
  if (!down)
    m_suppressed.erase(std::string(key));
  return consumed;
}
void Manager::SetFocus(bool focused)
{
  std::lock_guard lock(m_mutex);
  m_focused = focused;
  if (!focused)
  {
    End(false, true);
    End(true, true);
    m_suppressed.clear();
    m_pressed.clear();
    m_modifiers = 0;
  }
}
void Manager::FocusLost()
{
  SetFocus(false);
}
bool Manager::IsSuppressed(std::string_view key) const
{
  std::lock_guard lock(m_mutex);
  return m_focused && m_suppressed.contains(key);
}
std::optional<bool> Manager::GetWindowKeyState(std::string_view key) const
{
  std::lock_guard lock(m_mutex);
  if (!m_configured || (!IsKey(key) && key != "Shift" && key != "Ctrl" && key != "Alt"))
    return std::nullopt;
  return m_focused && m_pressed.contains(key) && !m_suppressed.contains(key);
}
Manager& Get()
{
  static Manager manager;
  return manager;
}
}  // namespace InputCommon::Shortcuts
