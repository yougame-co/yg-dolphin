// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

// Machine-readable status for the YouGame desktop app, one line each on stdout in a session:
//   orca state booting|lobby|playing|ended, and drop-in's (ORCA.md "Drop-in"): joining <percent>,
//   friend-joining, friend-joined, friend-left <why>, host-left, left, no-room
//   orca error <code> <sentence for the player>
//   orca menu <event>, a pick in the game's own Online menu: online casual local, online ranked
//   local, online friends
//   orca shaders <compiled> <total>, while the boot compiles shaders before the first frame
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
// "orca menu <event>": the player picked something in the game's own Online menu (ORCA.md "Online
// menu"): "online casual local", "online ranked local", "online friends". An event, never a state:
// each pick prints once.
void Menu(std::string_view event);
// An error the session survives: with the app's "caps join" it is printed and Orca plays on solo
// (the exit code stays 0, the embed "error" line isn't sent); without, it is Error().
void Report(std::string_view code, std::string_view sentence);

// Capabilities (ORCA.md, "Embedding"): what this build implements, printed once as
// "orca caps <list>" before "ready", and what the app said it understands ("caps <list>" on
// stdin). A feature is on only when both list it. "pause" needs no answer: it says this Orca
// pauses a session while the player's game is alone in it (older ones answer "unsupported pause").
// "delay": the app may send "delay auto|1..6", the player's own input delay (ORCA.md).
// "perf": the app may send "perf off|fps|detailed", the overlay's frame meter (ORCA.md).
inline constexpr const char* CAPS = "join leave stats pause delay perf";
void PrintCaps();
void SetAppCaps(std::string_view list);
bool Cap(std::string_view name);
// "orca stats <json>" (with the "stats" capability).
void Stats(std::string_view json);
// "orca shaders <compiled> <total>", about once a second while the boot waits for the pipelines it
// compiles before the first frame (VideoCommon/ShaderCache.cpp, WaitForBootCompile), so the app
// can tell a boot that is compiling from one that hangs; then once more as the wait ends. Only
// before "ready", and only when that wait lasts a second or more.
void Shaders(std::size_t compiled, std::size_t total);
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
