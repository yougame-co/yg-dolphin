// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

// Embed mode (ORCA.md "Embedding"): Orca draws inside the YouGame desktop app's window instead of a
// window of its own. The app starts it with
//   --embed --parent <handle> --rect X Y W H     (or --rect X,Y,W,H)
// steers it with one command per line on stdin and reads one event per line on stdout, the same
// protocol as the app's libretro host (desktop/native/libretro/host.cpp).
namespace Embed
{
struct Rect
{
  int x = 0;
  int y = 0;
  int w = 1;
  int h = 1;
};

struct Options
{
  bool enabled = false;
  // HWND on Windows, CGWindowID (the window number) on macOS, as a decimal number.
  unsigned long long parent = 0;
  Rect rect;
  // The --rect asked for was TooSmall (and rect is that one at MIN_SIDE): the view starts hidden.
  bool too_small = false;
};

// Takes --embed, --parent and --rect out of argv (so the regular parser never sees them). Returns
// nullopt with a message in *error when they are malformed.
std::optional<Options> TakeArguments(std::vector<char*>* argv, std::string* error);

// "rect X Y W H" (also "X,Y,W,H"); w and h at least 1.
std::optional<Rect> ParseRect(const std::string& text);

// A rect with a side under MIN_SIDE pixels is no place for the game: a `rect` command that small
// hides it (the window keeps its last size) until a big enough one arrives, and a --rect that small
// opens the window at MIN_SIDE, hidden. Pages that clipped the player box to its visible part used to
// shrink the window to nothing while scrolling, and the overlay aborted in a window that thin.
constexpr int MIN_SIDE = 32;
bool TooSmall(const Rect& rect);

// The protocol gets stdout to itself: fd 1 is duplicated for it, then pointed at stderr, so the
// log (Windows writes it to stdout) and anything else printed never lands among the replies.
// Returns the protocol stream. Call once, first thing.
std::FILE* ClaimStdout();

// One protocol line on stdout (a newline is added; embedded newlines become spaces).
void Out(const std::string& line);

// Reads stdin on a thread of its own; a closed stdin reads as "quit".
void StartReading();
// The commands read since the last call, oldest first.
std::vector<std::string> TakeCommands();
}  // namespace Embed
