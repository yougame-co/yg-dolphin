// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdio>
#include <functional>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"

namespace Orca::RebuildStandard
{
int Run(const std::string& image, const std::string& layout,
        const std::optional<std::string>& reference, const std::string& output, std::FILE* out,
        const std::function<void(u64 done, u64 total)>& progress = {});
// Dispatch only from main, before configuration, logging or user-directory initialization.
std::optional<int> RunCommandLine(int argc, char* argv[]);
bool IsCancelled();
}  // namespace Orca::RebuildStandard
