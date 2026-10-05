// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

// Machine-readable status for the YouGame desktop app, one line each on stdout in a session:
//   orca state booting|lobby|playing|ended, and drop-in's (ORCA.md "Drop-in"): joining <percent>,
//   friend-joining, friend-joined, friend-left <why>, host-left, left, left lobby (With Anyone >
//   Casual or Ranked was picked in a host's friends lobby, by any player in it, and the host left
//   its friends for the queue; its `orca menu online ...` follows; each friend gets host-left and
//   the same `orca menu online ...` once home), no-room (a former joiner in a fight, until it is
//   back on the menus and opens a room of its own), and a held join's
//   (ORCA.md "Boot and friends from the menus"): friend-holding (the host: a friend waits until its
//   game is back in the menus), friend-waiting (the joiner: the host plays a single-player mode)
//   orca error <code> <sentence for the player>
//   orca menu <event>, a pick in the game's own Online menu: online casual, online ranked (with the
//   app's "host"; "... local" without it), online friends, cancel (ORCA.md "Matchmaking")
//   orca result <json>, a verdict of the room in a matchmade room (with the app's "results")
//   orca queue ready <casual|ranked> <char> <costume> | unready | skip | timeout me|them, the
//   queue's character select (with the app's "queue2", UX/Queue.h; through Line())
//   orca shaders <compiled> <total>, while the boot compiles shaders before the first frame
// Codes: boot, cancelled (Orca.app's disc picker closed without a disc), disc_missing and
// disc_unreadable (the -e file isn't there, or won't open: the sentence is its path), disc_revision,
// profile, settings, room_mismatch, room_full, kicked, network, peer_left, desync, internal.
// Only the first error is printed: it is the one that explains the exit. Outside a session these
// print nothing.
namespace Orca::Status
{
void State(std::string_view state);
// A state printed even when it repeats the current one ("left" answers every leave command).
void Event(std::string_view state);
void Error(std::string_view code, std::string_view sentence);
// "orca menu <event>": the player picked something in the game's own Online menu (ORCA.md "Online
// menu"): "online casual", "online ranked" (with the app's "host"; "online casual local" and
// "online ranked local" without it), "online friends", and "cancel" (the player backed out of a
// search). An event, never a state: each pick prints once.
void Menu(std::string_view event);
// "orca result <json>" (with the "results" capability): a matchmade room's verdict on a game or a
// ranked set (ORCA.md "Matchmaking"). The whole line stays within 300 bytes (the app's limit).
void Result(std::string_view json);
// A line of the embed protocol as it is ("unsupported host"), on the status stream.
void Line(std::string_view line);
// An error the session survives: with the app's "caps join" it is printed and Orca plays on solo
// (the exit code stays 0, the embed "error" line isn't sent); without, it is Error().
void Report(std::string_view code, std::string_view sentence);

// Capabilities (ORCA.md, "Embedding"): what this build implements, printed once as
// "orca caps <list>" before "ready", and what the app said it understands ("caps <list>" on
// stdin). A feature is on only when both list it. "pause" needs no answer: it says this Orca
// pauses a session while the player's game is alone in it (older ones answer "unsupported pause").
// "delay": the app may send "delay auto|1..6", the player's own input delay (ORCA.md).
// "perf": the app may send "perf off|fps|detailed", the overlay's frame meter (ORCA.md).
// "direct": the app may send "direct on|off", the player's switch for direct links (ORCA.md).
// "host": the app may send "host <code>" (host the room the queue matched this game into) and
// "queue-cancel" (the search ended); the Online menu's Casual and Ranked print without "local".
// "chat": the app may send "chat <name> <text>", a line of the page's chat shown as a toast while
// Orca fills the screen (UX/Chat.h; never printed or logged).
// "yougame": the app may send "orb <x> <y> <size> <lit> <away> <badge>" / "orb blink" / "orb off"
// and "notice <kind> <name> <text> [<key> <verb>]": the YouGame button over the game, where the
// page's own stands under Orca's window, and the overlay's lines beside it (UX/YgOrb.h); a press
// on the button prints "orca orb" where Orca's window takes the mouse (Windows).
// "results": "orca result" lines, offered only for a profile whose result reader is verified
// (UX/Results.h, ResultsVerified); the room hears the results either way (ORCA.md "Matchmaking").
inline constexpr const char* CAPS = "join leave stats pause delay perf direct host chat yougame";
inline constexpr const char* RESULTS_CAP = "results";
// "locks": a queue room's game carries the online rules (UX/OnlineRules.h: rules locked, casual's
// random legal stage, no way out of the character and stage select), offered for a profile with a
// ruleset (Brawl rev 2, Project+). Information for the page: the locks don't wait for its answer.
inline constexpr const char* LOCKS_CAP = "locks";
// "queue2": the queue's character select, Slippi-style (UX/Queue.h): With Anyone lands on the
// player's own character select, `orca queue ready|unready|skip|timeout` lines, the app's
// `queue rating <n|->`, and a matchmade room's ready timer. Offered with "locks" (a ruleset); with
// the app's answer the Casual and Ranked picks wait for `orca queue ready` instead of searching.
inline constexpr const char* QUEUE2_CAP = "queue2";
// What this build offers for the running profile: CAPS, "results" where its reader is verified,
// and "locks" where it has a ruleset.
std::string OfferedCaps();
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
