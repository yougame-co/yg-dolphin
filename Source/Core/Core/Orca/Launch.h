// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

// orca-launch.ini: how Orca.app starts a launcher profile when the YouGame app runs it with no
// arguments. The file sits next to Orca.app, in the folder the app unpacked the native build into,
// with the mod's files beside it:
//
//   [Launch]
//   Profile = PPLUS32
//   Executable = pplus/Project+ Netplay Launcher.dol
//
// Profile names a launcher profile in Sys/Orca (ORCA_PROFILE); Executable is the loader to boot
// (-e), as a path relative to that folder. The profile pins the loader, the disc and the SD card
// image next to the loader by hash, so the file only says where they are, never what they may be.
// The parser is strict: one [Launch] section, those two keys once each, nothing else; an absolute
// path, a "." or ".." component, a backslash or a control character is refused, and the loader
// must resolve (symlinks included) inside the folder.
namespace Orca
{
inline constexpr std::string_view LAUNCH_INI = "orca-launch.ini";

struct LaunchSpec
{
  std::string profile;
  // Relative to the folder holding the ini, as written.
  std::string executable;
};

// Parses the file's text; nullopt and the reason when it's malformed.
std::optional<LaunchSpec> ParseLaunchIni(std::string_view text, std::string* error);

// The spec in `folder`/orca-launch.ini with its loader as an absolute path checked to be a file
// inside `folder`. Nullopt with an empty error when there is no such file (a plain Orca.app build),
// nullopt with the reason when there is one and it's wrong.
std::optional<LaunchSpec> ReadLaunchIni(const std::string& folder, std::string* error);
}  // namespace Orca
