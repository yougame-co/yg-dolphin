// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Branding.h"

#include "Core/ConfigManager.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Online.h"

namespace Orca
{
std::string WindowTitle()
{
  std::string title = "Orca";
  // A launcher profile's game is the mod it boots, not the disc's (Project+, not Brawl).
  const Profile* profile = ActiveProfile();
  if (profile && !profile->title.empty())
    title += " — " + profile->title;
  else if (const std::string& game = SConfig::GetInstance().GetTitleDescription(); !game.empty())
    title += " — " + game;
  if (std::string status = Online::StatusLine(); !status.empty())
  {
    // Not the room code: window pickers (streaming, screen sharing) show titles, and the code
    // lets anyone into the room. The on-screen display and the YouGame page show it instead.
    if (status.starts_with("Room "))
    {
      if (const auto colon = status.find(": "); colon != std::string::npos)
        status.erase(0, colon + 2);
    }
    title += " — " + status;
  }
  return title;
}

std::string BrandText(std::string_view text)
{
  std::string branded(text);
  if (!SessionActive())
    return branded;
  static constexpr std::string_view UPSTREAM = "Dolphin";
  static constexpr std::string_view PRODUCT = "Orca";
  for (std::size_t at = branded.find(UPSTREAM); at != std::string::npos;
       at = branded.find(UPSTREAM, at + PRODUCT.size()))
  {
    branded.replace(at, UPSTREAM.size(), PRODUCT);
  }
  return branded;
}
}  // namespace Orca
