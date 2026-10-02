// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Probe.h"

#include <chrono>
#include <cstdlib>
#include <thread>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"
#include "Core/Orca/UX/Overlay.h"

// Research tool for the name tags (ORCA_ONLINE_UX.md 1C), never on in a real session:
// ORCA_UX_PROBE=<file> runs one command per line at a frame boundary, counted from entry into a
// scene (Brawl rev 2's scene manager), like the harness's input scripts:
//   @<scene> <frame> write <addr> <hex bytes>      e.g. @muMenuMain 20 write 90180c10 0000
//   @<scene> <frame> utf16 <addr> <text>           text as UTF-16BE with a 0 terminator
//   @<scene> <frame> dump <addr> <length> <name>   to <user dir>/probe-<name>.bin
//   @<scene> <frame> stall <ms> 0                  block the CPU thread like a session stall,
//                                                  calling RepresentDuringStall every 10 ms
// <addr> is hex, or [hex]+hex for one pointer hop (e.g. [805a00e0]+28).
namespace Orca::UX
{
namespace
{
struct Command
{
  std::string scene;
  int frame = 0;
  std::string verb;
  std::string addr;
  std::string arg;
  std::string arg2;
  bool done = false;
};

std::vector<Command> Load()
{
  std::vector<Command> out;
  const char* path = std::getenv("ORCA_UX_PROBE");
  if (!path)
    return out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line))
  {
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream s(line);
    Command c;
    s >> c.scene >> c.frame >> c.verb >> c.addr >> c.arg;
    std::getline(s, c.arg2);
    if (!c.arg2.empty() && c.arg2[0] == ' ')
      c.arg2.erase(0, 1);
    if (c.scene.starts_with("@"))
      c.scene.erase(0, 1);
    out.push_back(c);
  }
  return out;
}

std::string SceneName(const Core::CPUThreadGuard& guard)
{
  const u32 manager = PowerPC::MMU::HostRead<u32>(guard, 0x805a0060);
  if (manager < 0x80000000u)
    return {};
  const u32 scene = PowerPC::MMU::HostRead<u32>(guard, manager + 0x4);
  if (scene < 0x80000000u)
    return {};
  const u32 name_ptr = PowerPC::MMU::HostRead<u32>(guard, scene);
  if (name_ptr < 0x80000000u)
    return {};
  std::string name;
  for (u32 i = 0; i < 32; ++i)
  {
    const char c = static_cast<char>(PowerPC::MMU::HostRead<u8>(guard, name_ptr + i));
    if (c == '\0')
      break;
    name.push_back(c);
  }
  return name;
}

u32 Address(const Core::CPUThreadGuard& guard, const std::string& expr)
{
  if (expr.starts_with("["))
  {
    const size_t close = expr.find(']');
    const u32 at = static_cast<u32>(std::strtoul(expr.substr(1, close - 1).c_str(), nullptr, 16));
    u32 offset = 0;
    if (close + 1 < expr.size() && expr[close + 1] == '+')
      offset = static_cast<u32>(std::strtoul(expr.substr(close + 2).c_str(), nullptr, 16));
    return PowerPC::MMU::HostRead<u32>(guard, at) + offset;
  }
  return static_cast<u32>(std::strtoul(expr.c_str(), nullptr, 16));
}
}  // namespace

void ProbeFrame(const Core::CPUThreadGuard& guard)
{
  static std::vector<Command> commands = Load();
  static int frame = 0;
  ++frame;
  if (commands.empty())
    return;
  static std::string scene;
  static int scene_start = 0;
  const std::string now = SceneName(guard);
  if (now != scene)
  {
    scene = now;
    scene_start = frame;
  }
  const int rel = frame - scene_start;
  for (Command& c : commands)
  {
    if (c.done || c.scene != scene || c.frame != rel)
      continue;
    c.done = true;
    if (c.verb == "stall")
    {
      const int ms = std::atoi(c.addr.c_str());
      for (int waited = 0; waited < ms; waited += 10)
      {
        RepresentDuringStall(waited);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      NOTICE_LOG_FMT(ROLLBACK, "Probe {} {}: stalled {} ms", scene, rel, ms);
      continue;
    }
    const u32 addr = Address(guard, c.addr);
    if (c.verb == "write")
    {
      const std::string hex = c.arg + c.arg2;
      std::string bytes;
      for (char ch : hex)
        if (std::isxdigit(static_cast<unsigned char>(ch)))
          bytes.push_back(ch);
      for (size_t i = 0; i + 1 < bytes.size(); i += 2)
        PowerPC::MMU::HostWrite<u8>(
            guard, static_cast<u8>(std::strtoul(bytes.substr(i, 2).c_str(), nullptr, 16)),
            addr + static_cast<u32>(i / 2));
    }
    else if (c.verb == "utf16")
    {
      const std::string text = c.arg + (c.arg2.empty() ? "" : " " + c.arg2);
      u32 a = addr;
      for (unsigned char ch : text)
      {
        PowerPC::MMU::HostWrite<u16>(guard, ch, a);
        a += 2;
      }
      PowerPC::MMU::HostWrite<u16>(guard, 0, a);
    }
    else if (c.verb == "dump")
    {
      const u32 length = static_cast<u32>(std::strtoul(c.arg.c_str(), nullptr, 16));
      std::vector<u8> data(length);
      for (u32 i = 0; i < length; ++i)
        data[i] = PowerPC::MMU::HostRead<u8>(guard, addr + i);
      File::IOFile(File::GetUserPath(D_USER_IDX) + fmt::format("probe-{}.bin", c.arg2), "wb")
          .WriteBytes(data.data(), data.size());
    }
    NOTICE_LOG_FMT(ROLLBACK, "Probe {} {}: {} {:08x} {} {}", scene, rel, c.verb, addr, c.arg,
                   c.arg2);
  }
}
}  // namespace Orca::UX
