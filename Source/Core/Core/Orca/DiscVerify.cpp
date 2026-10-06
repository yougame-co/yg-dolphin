// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/DiscVerify.h"

#include <cstring>
#include <memory>

#include <fmt/format.h>

#include "DiscIO/Volume.h"

namespace Orca::DiscVerify
{
namespace
{
using Severity = DiscIO::VolumeVerifier::Severity;

bool IsSerious(Severity severity)
{
  return severity == Severity::Medium || severity == Severity::High;
}

std::string_view SeverityWord(Severity severity)
{
  switch (severity)
  {
  case Severity::High:
    return "high";
  case Severity::Medium:
    return "medium";
  default:
    return "low";
  }
}

bool IsControl(char c)
{
  const auto u = static_cast<unsigned char>(c);
  return u < 0x20 || u == 0x7f;
}

std::string OneLine(std::string_view text)
{
  std::string line;
  for (const char c : text)
  {
    if (c != ' ' && !IsControl(c))
      line += c;
    else if (!line.empty() && line.back() != ' ')
      line += ' ';
  }
  if (!line.empty() && line.back() == ' ')
    line.pop_back();
  return line;
}
}  // namespace

std::optional<std::string> TakeArgument(std::vector<char*>* argv, std::string* error)
{
  std::string path;
  std::vector<char*> kept;
  for (size_t i = 0; i < argv->size(); ++i)
  {
    char* const arg = (*argv)[i];
    if (i == 0 || !arg || std::strcmp(arg, "--verify") != 0)
    {
      kept.push_back(arg);
      continue;
    }
    const char* const value = i + 1 < argv->size() ? (*argv)[i + 1] : nullptr;
    if (!value || !*value)
    {
      *error = "--verify needs the path of a disc image";
      return std::nullopt;
    }
    path = value;
    ++i;
  }
  *argv = std::move(kept);
  return path;
}

std::size_t CountSerious(std::span<const Problem> problems)
{
  std::size_t serious = 0;
  for (const Problem& problem : problems)
  {
    if (IsSerious(problem.severity))
      ++serious;
  }
  return serious;
}

int ExitCode(std::size_t serious)
{
  return serious ? EXIT_DAMAGED : EXIT_OK;
}

std::string ProgressLine(u64 done, u64 total)
{
  return fmt::format("orca verify progress {} {}", done, total);
}

std::string ProblemLine(const Problem& problem)
{
  return fmt::format("orca verify problem {} {}", SeverityWord(problem.severity),
                     OneLine(problem.text));
}

std::string VerdictLine(std::size_t serious)
{
  return serious ? fmt::format("orca verify damaged {}", serious) : "orca verify ok";
}

std::string UnreadableLine(std::string_view path)
{
  // The path as given, on one line.
  std::string line = fmt::format("orca verify unreadable {}", path);
  for (char& c : line)
  {
    if (IsControl(c))
      c = ' ';
  }
  return line;
}

bool ProgressPacer::Due(std::chrono::steady_clock::time_point now)
{
  if (m_last && now - *m_last < std::chrono::seconds(1))
    return false;
  m_last = now;
  return true;
}

int Run(const std::string& path, std::FILE* out)
{
  const auto print = [out](const std::string& line) {
    fmt::print(out, "{}\n", line);
    std::fflush(out);
  };

  const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(path);
  if (!volume)
  {
    print(UnreadableLine(path));
    return EXIT_UNREADABLE;
  }

  // No Redump lookup and no whole-disc hashes: the Wii hash tree is the integrity check.
  DiscIO::VolumeVerifier verifier(*volume, false, {});
  verifier.Start();
  ProgressPacer pacer;
  while (verifier.GetBytesProcessed() != verifier.GetTotalBytes())
  {
    if (pacer.Due(std::chrono::steady_clock::now()))
      print(ProgressLine(verifier.GetBytesProcessed(), verifier.GetTotalBytes()));
    verifier.Process();
  }
  verifier.Finish();

  const std::vector<Problem>& problems = verifier.GetResult().problems;
  for (const Problem& problem : problems)
    print(ProblemLine(problem));
  const std::size_t serious = CountSerious(problems);
  print(VerdictLine(serious));
  return ExitCode(serious);
}
}  // namespace Orca::DiscVerify
