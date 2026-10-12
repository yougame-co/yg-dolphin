// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include "Core/Orca/Engine/Module.h"

namespace Orca
{
struct Profile;
}

// Which of the builtin module's games a session runs (Orca split, S3 "game ids out"): the one place
// that compares game ids, disc ids, revisions and mod.json builtins. Every game file and the engine
// ask the module (Module::Describe, Active, ProfileRuleset) instead.
namespace Orca::UX::BrawlGame
{
// Mod.h's builtin names, and the legacy-only third game: another launcher build that boots Brawl
// rev 2 (not Project+'s PPLUS32), which gets the menus, name tags and results but no ruleset.
inline constexpr std::string_view BRAWL_REV2 = "brawl-rev2";
inline constexpr std::string_view BRAWL_REV2_PPLUS = "brawl-rev2-pplus";
inline constexpr std::string_view BRAWL_REV2_LAUNCHER = "brawl-rev2-launcher";

// The game `profile` runs (null: no session). Static for a boot: the profile and mod.json only,
// never memory.
Orca::Engine::Manifest Identify(const Orca::Profile* profile);
// Host only: whether the harness can read Brawl rev 2's scene names (Module::ScenesReadable), also
// without a profile when Dolphin itself runs Brawl rev 2 (`running_game`, `running_revision`).
bool ScenesReadable(const Orca::Profile* profile, std::string_view running_game,
                    u16 running_revision);
// Whether the module's code applies to memory now, for the game `game` describes: in mod mode while
// the builtin's guard words held at the last Mod::RefreshGuards; in legacy mode always.
bool On(const Orca::Engine::Manifest& game);
}  // namespace Orca::UX::BrawlGame
