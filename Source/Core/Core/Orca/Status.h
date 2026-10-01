// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

// Machine-readable status for the YouGame desktop app, one line each on stdout in a session:
//   orca state booting|lobby|playing|ended
//   orca error <code> <sentence for the player>
// Codes: boot, disc_revision, profile, settings, room_mismatch, room_full, kicked, network,
// peer_left, desync, internal.
// Only the first error is printed: it is the one that explains the exit. Outside a session these
// print nothing.
namespace Orca::Status
{
void State(std::string_view state);
void Error(std::string_view code, std::string_view sentence);
// 0 after a normal leave; 1 once an error was reported.
int ExitCode();
// At process exit: a session that never got past booting without saying why reports a boot error;
// then "ended". Returns ExitCode().
int Finish();
}  // namespace Orca::Status
