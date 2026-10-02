// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdio>
#include <string>
#include <string_view>

// Machine-readable status for the YouGame desktop app, one line each on stdout in a session:
//   orca state booting|lobby|playing|ended, and drop-in's (ORCA.md "Drop-in"): joining <percent>,
//   friend-joining, friend-joined, friend-left <why>, host-left, left, no-room
//   orca error <code> <sentence for the player>
// Codes: boot, cancelled (Orca.app's disc picker closed without a disc), disc_revision, profile,
// settings, room_mismatch, room_full, kicked, network, peer_left, desync, internal.
// Only the first error is printed: it is the one that explains the exit. Outside a session these
// print nothing.
namespace Orca::Status
{
void State(std::string_view state);
// A state printed even when it repeats the current one ("left" answers every leave command).
void Event(std::string_view state);
void Error(std::string_view code, std::string_view sentence);
// An error the session survives: with the app's "caps join" it is printed and Orca plays on solo
// (the exit code stays 0, the embed "error" line isn't sent); without, it is Error().
void Report(std::string_view code, std::string_view sentence);

// Capabilities (ORCA.md, "Embedding"): what this build implements, printed once as
// "orca caps <list>" before "ready", and what the app said it understands ("caps <list>" on
// stdin). A feature is on only when both list it.
inline constexpr const char* CAPS = "join leave stats";
void PrintCaps();
void SetAppCaps(std::string_view list);
bool Cap(std::string_view name);
// "orca stats <json>" (with the "stats" capability).
void Stats(std::string_view json);
// 0 after a normal leave; 1 once an error was reported.
int ExitCode();
// The game reached its first frame (called at every frame boundary; cheap after the first).
void GameStarted();
// Where the lines go (default stdout). The embed mode gives its protocol stream: stdout itself
// then points at stderr, so nothing else (a log line) can land in the protocol.
void SetOutput(std::FILE* out);
// Also told about the error that gets printed (the embed mode repeats it in the app's "error" form).
void SetErrorListener(void (*listener)(std::string_view code, std::string_view sentence));
// At process exit: a session whose game never reached a frame, without saying why, reports a boot
// error; then "ended". Returns ExitCode().
int Finish();
}  // namespace Orca::Status
