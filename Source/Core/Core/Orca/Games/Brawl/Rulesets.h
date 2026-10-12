// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/RankedSteps.h"

// The builtin module's rulesets, as data (Orca split S5): Rulesets/brawl-supernova-2025.json and
// Rulesets/pplus-2024.json, compiled in byte for byte and read on first use through the engine's
// checked reader (Engine/Ruleset.h), the same as any pack's ruleset will be. A file the reader
// refuses is a build defect (OrcaRulesets.BuiltinsAreTheOldTables catches it): the ruleset is then
// an empty one with no stages, and the error is logged once.
namespace Orca::UX::Rulesets
{
// Brawl, Supernova 2025 (MatchBlock ruleset 1).
const Ranked::Ruleset& Brawl2025();
// Project+ 2024 (MatchBlock ruleset 2).
const Ranked::Ruleset& PPlus2024();
// The builtin ruleset with MatchBlock ruleset byte `block`, or nullptr (0, or unknown).
const Ranked::Ruleset* ByBlock(u8 block);
}  // namespace Orca::UX::Rulesets
