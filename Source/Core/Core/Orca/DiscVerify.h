// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/VolumeVerifier.h"

// `Orca --verify <disc>`: Dolphin's integrity check of a disc image (the Wii hash tree; no Redump,
// no whole-disc hashes), without booting it. One line each on stdout:
//   orca verify progress <bytes done> <bytes total>   at most once a second
//   orca verify problem <low|medium|high> <text>      one per problem the verifier reports
//   orca verify ok | orca verify damaged <n> | orca verify unreadable <path>
// Damaged means n Medium or High problems. Exit codes: 0 ok, 2 damaged, 1 unreadable.
namespace Orca::DiscVerify
{
using Problem = DiscIO::VolumeVerifier::Problem;

inline constexpr int EXIT_OK = 0;
inline constexpr int EXIT_UNREADABLE = 1;
inline constexpr int EXIT_DAMAGED = 2;

// Removes "--verify <path>" from argv. Returns the path, "" without the flag, or nullopt and sets
// *error when the path is missing.
std::optional<std::string> TakeArgument(std::vector<char*>* argv, std::string* error);

// The Medium and High problems: any of them and the disc is damaged.
std::size_t CountSerious(std::span<const Problem> problems);
int ExitCode(std::size_t serious);

std::string ProgressLine(u64 done, u64 total);
// The text goes on one line: each run of spaces, line breaks and other control characters becomes
// one space.
std::string ProblemLine(const Problem& problem);
std::string VerdictLine(std::size_t serious);
std::string UnreadableLine(std::string_view path);

// Lets a progress line through at most once a second.
class ProgressPacer
{
public:
  bool Due(std::chrono::steady_clock::time_point now);

private:
  std::optional<std::chrono::steady_clock::time_point> m_last;
};

// Opens the disc and verifies it, printing the lines above to `out`. Returns the exit code.
int Run(const std::string& path, std::FILE* out);
}  // namespace Orca::DiscVerify
