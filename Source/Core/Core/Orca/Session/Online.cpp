// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/Online.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <xxh3.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/Orca/Session/YouGameRoom.h"

namespace Orca::Online
{
namespace
{
std::mutex s_mutex;
// Shared so that a caller still holding the room (WaitForMatch, a status poll) keeps it alive
// while Shutdown runs on another thread.
std::shared_ptr<Net::YouGameRoom> s_room;

std::string Env(const char* name)
{
  return GetEnv(name);
}

std::shared_ptr<Net::YouGameRoom> Room()
{
  std::lock_guard lock(s_mutex);
  return s_room;
}
}  // namespace

bool Enabled()
{
  if (!SessionActive())
    return false;
  return (!Env("YOUGAME_BRIDGE").empty() && !Env("YOUGAME_TOKEN").empty()) ||
         !Env("ORCA_TEST_DEV_GAME").empty();
}

std::string CompatibilityKey()
{
  std::string parts = fmt::format("orca|{}|", Common::GetScmRevGitStr());
  if (const Profile* profile = ActiveProfile())
  {
    parts += fmt::format("{}|{}|", profile->game_id, profile->revision);
    // The data files that reach emulation besides the code: the profile itself, and every game
    // settings INI Dolphin loads for this disc, shipped and the player's own (a local edit there
    // changes what the game runs with, so it must change the key).
    std::vector<std::string> files{File::GetSysDirectory() + "Orca/" + profile->game_id + ".ini"};
    for (const std::string& name :
         ConfigLoaders::GetGameIniFilenames(profile->game_id, profile->revision))
    {
      files.push_back(File::GetSysDirectory() + GAMESETTINGS_DIR DIR_SEP + name);
      files.push_back(File::GetUserPath(D_GAMESETTINGS_IDX) + name);
    }
    for (const std::string& path : files)
    {
      std::string contents;
      if (File::ReadFileToString(path, contents))
        parts += fmt::format("{:016x}|", XXH3_64bits(contents.data(), contents.size()));
      else
        parts += "-|";
    }
    for (const auto& [location, value] : profile->forced)
      parts += fmt::format("{}.{}.{}={}|", static_cast<int>(location.system), location.section,
                           location.key, value);
  }
  // The host CPU architecture: Jit64 and JitArm64 are not yet shown to compute the same results
  // (with inaccurate NaNs, for one, x86-64 and ARM64 produce default NaNs of opposite sign). Until a
  // cross-platform test proves otherwise, players only meet others on the same architecture.
#if defined(_M_ARM_64)
  parts += "arch=arm64|";
#elif defined(_M_X86_64)
  parts += "arch=x86-64|";
#else
  parts += "arch=other|";
#endif
  // The universal forced settings that change what the game computes.
  parts += fmt::format(
      "thread={}|fma={}|rtc={}:{}|si={},{},{},{}|efb={}|xfb={}|access={}|bbox={}|save={:016x}|test={}",
      Config::Get(Config::MAIN_CPU_THREAD), Config::Get(Config::SESSION_USE_FMA),
      Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE), Config::Get(Config::MAIN_CUSTOM_RTC_VALUE),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(0))),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(1))),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(2))),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(3))),
      Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM),
      Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM),
      Config::Get(Config::GFX_HACK_EFB_ACCESS_ENABLE), Config::Get(Config::GFX_HACK_BBOX_ENABLE),
      GetSeedSaveHash(), TestOverridesActive());
  // The disc image itself, when the desktop app hashed it (a modified disc with the right game ID
  // and revision would otherwise desync instead of being refused).
  parts += "|disc=" + Env("ORCA_DISC_SHA1");
  // The room allows 1-64 characters [a-zA-Z0-9._:-].
  return fmt::format("orca1:{:016x}", XXH3_64bits(parts.data(), parts.size()));
}

Net::Transport* WaitForMatch(Net::Config* config, const std::function<bool()>& stopping)
{
  {
    std::lock_guard lock(s_mutex);
    if (!s_room)
    {
      Net::RoomOptions options;
      options.room_code = Env("ORCA_ROOM");
      options.compatibility = CompatibilityKey();
      if (const Profile* profile = ActiveProfile())
      {
        std::string game = profile->game_id;
        Common::ToLower(&game);
        options.mode = "orca-" + game;
      }
      if (!Env("ORCA_NAME").empty())
        options.player_name = Env("ORCA_NAME").substr(0, 32);
      NOTICE_LOG_FMT(NETPLAY, "Orca: joining a YouGame room, compatibility {}",
                     options.compatibility);
      s_room = std::make_shared<Net::YouGameRoom>(std::move(options));
    }
  }
  const std::shared_ptr<Net::YouGameRoom> room = Room();
  if (!room)
    return nullptr;
  std::string last_status;
  Status::State("lobby");
  while (!stopping())
  {
    const Net::RoomState state = room->GetState();
    if (state == Net::RoomState::Playing)
    {
      Status::State("playing");
      config->seats = 2;
      config->local_seat = room->Seat();
      NOTICE_LOG_FMT(NETPLAY, "Orca: match on room {}, port {}", room->Code(), room->Seat() + 1);
      Core::DisplayMessage(fmt::format("Playing on port {}", room->Seat() + 1), 4000);
      return room.get();
    }
    if (state == Net::RoomState::Ended)
    {
      ERROR_LOG_FMT(NETPLAY, "Orca: {}", room->Status());
      Core::DisplayMessage(room->Status(), 10000);
      if (const std::string code = room->ErrorCode(); !code.empty())
        Status::Error(code, room->Status());
      return nullptr;
    }
    if (std::string status = room->Status(); status != last_status)
    {
      NOTICE_LOG_FMT(NETPLAY, "Orca: {}", status);
      // On screen too, until the next line replaces it: the room code is what a friend needs.
      Core::DisplayMessage(status, 60000);
      last_status = std::move(status);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return nullptr;
}

u32 Seed()
{
  const auto room = Room();
  return room ? room->Seed() : 0;
}

int RoundTripMs(u32* sequence)
{
  const auto room = Room();
  return room ? room->RoundTripMs(sequence) : -1;
}

std::string StatusLine()
{
  const auto room = Room();
  return room ? room->Status() : "";
}

void ReportSessionEnd(const std::string& session_error)
{
  const auto room = Room();
  // Inputs that disagree, or a checksum that does: the games went apart. The friend hears it
  // before this side leaves, so both report the same thing.
  const bool desync = session_error.starts_with("Desync") ||
                      session_error.find("changed its input") != std::string::npos ||
                      session_error.find("arrived after") != std::string::npos ||
                      session_error.starts_with("No local input");
  if (desync)
  {
    if (room)
      room->SetLeaveReason("desync");
    Status::Error("desync", "Your games went out of sync, so the match ended");
    return;
  }
  // The session ends when the room does: the room says why, unless this player left. The state
  // first: once Ended, the code is final.
  if (room && room->GetState() == Net::RoomState::Ended)
  {
    if (const std::string code = room->ErrorCode(); !code.empty())
      Status::Error(code, room->Status());
    return;
  }
  if (session_error.empty())
    return;
  // The emulator side failed (a snapshot that wouldn't save or load, lost frame alignment), or
  // the friend sent something no honest Orca sends.
  const bool from_peer = session_error.find("sent an invalid packet") != std::string::npos;
  Status::Error(from_peer ? "network" : "internal", session_error);
}

void ReportPeerStalled(std::string_view sentence)
{
  if (const auto room = Room())
    room->SetLeaveReason("stalled");
  Status::Error("peer_left", sentence);
}

void Shutdown()
{
  std::shared_ptr<Net::YouGameRoom> room;
  {
    std::lock_guard lock(s_mutex);
    room = std::move(s_room);
  }
  if (room)
    room->Leave();
}
}  // namespace Orca::Online
