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
  if (const std::string& game = SConfig::GetInstance().GetTitleDescription(); !game.empty())
    title += " — " + game;
  if (const std::string status = Online::StatusLine(); !status.empty())
    title += " — " + status;
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
