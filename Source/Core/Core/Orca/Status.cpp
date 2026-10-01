// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Status.h"

#include <cstdio>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include "Core/Orca/Profile.h"

namespace Orca::Status
{
namespace
{
std::mutex s_mutex;
std::string s_state;
bool s_errored = false;

void Print(const std::string& line)
{
  std::fputs(line.c_str(), stdout);
  std::fflush(stdout);
}

// One line per status: a sentence can't break the protocol.
std::string OneLine(std::string_view text)
{
  std::string out(text);
  for (char& c : out)
  {
    if (c == '\n' || c == '\r')
      c = ' ';
  }
  return out;
}
}  // namespace

void State(std::string_view state)
{
  if (!SessionActive())
    return;
  std::lock_guard lock(s_mutex);
  if (s_state == state)
    return;
  s_state = state;
  Print(fmt::format("orca state {}\n", state));
}

void Error(std::string_view code, std::string_view sentence)
{
  if (!SessionActive())
    return;
  std::lock_guard lock(s_mutex);
  if (s_errored)
    return;
  s_errored = true;
  Print(fmt::format("orca error {} {}\n", code, OneLine(sentence)));
}

int ExitCode()
{
  std::lock_guard lock(s_mutex);
  return s_errored ? 1 : 0;
}

int Finish()
{
  if (!SessionActive())
    return 0;
  bool stuck;
  {
    std::lock_guard lock(s_mutex);
    stuck = !s_errored && (s_state.empty() || s_state == "booting");
  }
  if (stuck)
    Error("boot", "Orca couldn't start the game");
  State("ended");
  return ExitCode();
}
}  // namespace Orca::Status
