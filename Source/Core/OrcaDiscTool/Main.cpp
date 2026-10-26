// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>

#include "Core/Orca/RebuildStandard.h"

#ifdef _WIN32
#include <windows.h>

#include "Common/StringUtil.h"
#define main app_main
#endif

int main(int argc, char* argv[])
{
  if (const auto result = Orca::RebuildStandard::RunCommandLine(argc, argv))
    return *result;
  std::fputs("usage: orca-disc-tool --disc-layout IMAGE --out LAYOUT.json\n"
             "       orca-disc-tool --rebuild-standard IMAGE --layout LAYOUT.json --out OUT.rvz "
             "[--against REFERENCE.json]\n",
             stderr);
  return 1;
}

#ifdef _WIN32
int wmain(int, wchar_t*[], wchar_t*[])
{
  auto args = Common::CommandLineToUtf8Argv(GetCommandLineW());
  std::vector<char*> argv;
  for (auto& arg : args)
    argv.push_back(arg.data());
  return main(static_cast<int>(argv.size()), argv.data());
}
#undef main
#endif
