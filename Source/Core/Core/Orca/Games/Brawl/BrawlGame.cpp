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

Orca::Engine::Manifest Game(std::string_view id, Orca::Engine::Ruleset ruleset, int compat)
{
  Orca::Engine::Manifest game;
  game.id = id;
  game.ruleset = ruleset;
  game.results = true;
  game.compat = compat;
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
      game = Game(BRAWL_REV2, Orca::Engine::Ruleset::Brawl, BRAWL_REV2_COMPAT);
      break;
    case Orca::Mod::Builtin::BrawlRev2PPlus:
      game = Game(BRAWL_REV2_PPLUS, Orca::Engine::Ruleset::PPlus, BRAWL_REV2_PPLUS_COMPAT);
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
      game = Game(BRAWL_REV2, Orca::Engine::Ruleset::Brawl, BRAWL_REV2_COMPAT);
    else if (profile->IsLauncher() && profile->disc == BRAWL_DISC)
    {
      game = profile->game_id == PPLUS_PROFILE ?
                 Game(BRAWL_REV2_PPLUS, Orca::Engine::Ruleset::PPlus, BRAWL_REV2_PPLUS_COMPAT) :
                 Game(BRAWL_REV2_LAUNCHER, Orca::Engine::Ruleset::None, BRAWL_REV2_LAUNCHER_COMPAT);
    }
  }
  // Brawl's disc, any revision and in either mode, builtin or not: its music is on stream voices.
  game.music =
      profile && (profile->IsLauncher() ? profile->disc : profile->game_id) == BRAWL_DISC;
  return game;
}

bool ScenesReadable(const Orca::Profile* profile, std::string_view running_game,
                    u16 running_revision)
{
  // Dolphin runs Brawl rev 2, a mod declares a builtin, or a launcher profile boots Brawl rev 2
  // (its running revision comes from the TMD and may not match, but the profile verified the disc).
  if (running_game == BRAWL_DISC && running_revision == BRAWL_REVISION)
    return true;
  if (profile && profile->mod && !Identify(profile).id.empty())
    return true;
  return profile && profile->IsLauncher() && profile->disc == BRAWL_DISC &&
         profile->revision == BRAWL_REVISION;
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
