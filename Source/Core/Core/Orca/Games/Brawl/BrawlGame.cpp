// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/BrawlGame.h"

#include <string_view>

#include "Core/Orca/Mod.h"
#include "Core/Orca/Profile.h"

namespace Orca::UX::BrawlGame
{
namespace
{
// Super Smash Bros. Brawl (NTSC-U), the revision whose executable the modules' addresses are for,
// and Project+'s launcher profile (Data/Sys/Orca/PPLUS32.ini).
constexpr std::string_view BRAWL_DISC = "RSBE01";
constexpr u16 BRAWL_REVISION = 2;
constexpr std::string_view PPLUS_PROFILE = "PPLUS32";

Orca::Engine::Manifest Game(std::string_view id, Orca::Engine::Ruleset ruleset)
{
  Orca::Engine::Manifest game;
  game.id = id;
  game.ruleset = ruleset;
  game.results = true;
  return game;
}
}  // namespace

Orca::Engine::Manifest Identify(const Orca::Profile* profile)
{
  Orca::Engine::Manifest game;
  if (profile && profile->mod)
  {
    // Mod mode: the builtin mod.json declares (ParseManifest allows one only on RSBE01 rev 2).
    switch (profile->mod->manifest.builtin)
    {
    case Orca::Mod::Builtin::BrawlRev2:
      game = Game(BRAWL_REV2, Orca::Engine::Ruleset::Brawl);
      break;
    case Orca::Mod::Builtin::BrawlRev2PPlus:
      game = Game(BRAWL_REV2_PPLUS, Orca::Engine::Ruleset::PPlus);
      break;
    default:
      break;
    }
  }
  else if (profile && profile->revision == BRAWL_REVISION)
  {
    // Legacy: Brawl rev 2 itself, or a launcher profile that boots it (Project+, or another build,
    // which plays no ruleset).
    if (!profile->IsLauncher() && profile->game_id == BRAWL_DISC)
      game = Game(BRAWL_REV2, Orca::Engine::Ruleset::Brawl);
    else if (profile->IsLauncher() && profile->disc == BRAWL_DISC)
    {
      game = profile->game_id == PPLUS_PROFILE ?
                 Game(BRAWL_REV2_PPLUS, Orca::Engine::Ruleset::PPlus) :
                 Game(BRAWL_REV2_LAUNCHER, Orca::Engine::Ruleset::None);
    }
  }
  // Brawl's disc, any revision and in either mode, builtin or not: its music is on stream voices.
  game.music =
      profile && (profile->IsLauncher() ? profile->disc : profile->game_id) == BRAWL_DISC;
  return game;
}

bool On(const Orca::Engine::Manifest& game)
{
  if (game.id.empty())
    return false;
  if (Orca::Mod::ActiveMod())
    return Orca::Mod::BuiltinOn(Orca::Mod::DeclaredBuiltin());
  return true;
}
}  // namespace Orca::UX::BrawlGame
