// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/OnlineMenu.h"

#include <string>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/NameTags.h"

namespace Orca::UX
{
namespace
{
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 MANAGER_SCENE = 0x4;
constexpr u32 MANAGER_EXIT_CODE = 0x284;
constexpr u32 SCENE_NAME_MAX = 32;

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

std::string SceneName(const GuestMemory& m)
{
  if (!Pointer(m, SCENE_MANAGER))
    return {};
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + MANAGER_SCENE))
    return {};
  const u32 scene = m.Read32(manager + MANAGER_SCENE);
  if (!Pointer(m, scene))
    return {};
  const u32 name = m.Read32(scene);
  std::string out;
  for (u32 i = 0; i < SCENE_NAME_MAX && m.Valid(name + i); ++i)
  {
    const char c = static_cast<char>(m.Read8(name + i));
    if (c == '\0')
      return out;
    out.push_back(c);
  }
  return {};  // not a name
}
}  // namespace

MenuState ReadMenuState(const GuestMemory& m)
{
  MenuState state;
  const std::string scene = SceneName(m);
  if (scene == "muMenuMain")
    state.where = MenuState::Where::Menu;
  else if (scene == "scMemoryChange")
    state.where = MenuState::Where::Leaving;
  if (state.where != MenuState::Where::Other)
  {
    const u32 manager = m.Read32(SCENE_MANAGER);
    if (Pointer(m, manager + MANAGER_EXIT_CODE))
      state.exit_code = m.Read32(manager + MANAGER_EXIT_CODE);
  }
  return state;
}

std::optional<OnlinePick> PickForExitCode(u32 exit_code)
{
  switch (exit_code)
  {
  case 24:
  case 25:
  case 26:
  case 27:
    return OnlinePick::Friends;
  case 30:
    return OnlinePick::Casual;
  case 31:
    return OnlinePick::Ranked;
  default:
    return std::nullopt;
  }
}

std::string_view MenuEventText(OnlinePick pick)
{
  switch (pick)
  {
  case OnlinePick::Casual:
    return "online casual local";
  case OnlinePick::Ranked:
    return "online ranked local";
  case OnlinePick::Friends:
    return "online friends";
  }
  return {};
}

std::optional<OnlinePick> OnlineMenuReader::Boundary(const MenuState& state, bool resimulating,
                                                     bool alone, u64 resyncs)
{
  // A re-run frame repeats a frame whose first run was already read (or that a rollback undid).
  if (resimulating)
    return std::nullopt;
  using Where = MenuState::Where;
  if (!m_started || resyncs != m_resyncs)
  {
    // A new start: whatever this boundary shows is where the game is, not something that happened.
    m_started = true;
    m_resyncs = resyncs;
    m_from_menu = state.where == Where::Menu;
    m_seen = state.where == Where::Leaving;
    return std::nullopt;
  }
  switch (state.where)
  {
  case Where::Menu:
    m_from_menu = true;
    m_seen = false;
    return std::nullopt;
  case Where::Other:
    m_from_menu = false;
    m_seen = false;
    return std::nullopt;
  case Where::Leaving:
    break;
  }
  // The menu's exit code shows from the first boundary between scenes; once per exit.
  if (!m_from_menu || m_seen || state.exit_code == 0)
    return std::nullopt;
  m_seen = true;
  const std::optional<OnlinePick> pick = PickForExitCode(state.exit_code);
  if (!pick || !alone)
    return std::nullopt;
  return pick;
}

void ReadOnlineMenu(const Core::CPUThreadGuard& guard, bool resimulating, bool alone)
{
  if (resimulating)
    return;
  // Brawl rev 2 itself or a launcher profile booting it (Project+): the addresses are that
  // executable's.
  const Orca::Profile* profile = Orca::ActiveProfile();
  if (!profile || profile->revision != 2 ||
      (profile->IsLauncher() ? profile->disc != "RSBE01" : profile->game_id != "RSBE01"))
  {
    return;
  }
  static OnlineMenuReader s_reader;
  GuardMemory memory(guard);
  const MenuState state = ReadMenuState(memory);
  if (const auto pick = s_reader.Boundary(state, false, alone, Orca::Events::Resyncs()))
  {
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: exit code {}: orca menu {}", state.exit_code,
                   MenuEventText(*pick));
    Orca::Status::Menu(MenuEventText(*pick));
  }
}
}  // namespace Orca::UX
