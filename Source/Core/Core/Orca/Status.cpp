// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Status.h"

#include <atomic>
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
std::atomic<bool> s_started{false};
std::FILE* s_out = nullptr;
void (*s_error_listener)(std::string_view, std::string_view) = nullptr;

void Print(const std::string& line)
{
  std::FILE* const out = s_out ? s_out : stdout;
  std::fputs(line.c_str(), out);
  std::fflush(out);
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
  if (s_error_listener)
    s_error_listener(code, OneLine(sentence));
}

void SetOutput(std::FILE* out)
{
  std::lock_guard lock(s_mutex);
  s_out = out;
}

void SetErrorListener(void (*listener)(std::string_view code, std::string_view sentence))
{
  std::lock_guard lock(s_mutex);
  s_error_listener = listener;
}

int ExitCode()
{
  std::lock_guard lock(s_mutex);
  return s_errored ? 1 : 0;
}

void GameStarted()
{
  s_started.store(true, std::memory_order_relaxed);
}

int Finish()
{
  if (!SessionActive())
    return 0;
  bool stuck;
  {
    std::lock_guard lock(s_mutex);
    stuck = !s_errored && !s_started.load(std::memory_order_relaxed);
  }
  if (stuck)
    Error("boot", "Orca couldn't start the game");
  State("ended");
  return ExitCode();
}
}  // namespace Orca::Status
