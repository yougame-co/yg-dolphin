// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/Online.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <xxh3.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include "Common/CPUDetect.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/YouGameRoom.h"

namespace Orca::Online
{
namespace
{
// How long Shutdown waits for rooms still being left in the background (a goodbye, a close
// handshake and the page's emptied room take about a second).
constexpr auto TEARDOWN_WAIT = std::chrono::seconds(2);
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

namespace
{
// How this machine's floating point treats NaNs in the guest's non-IEEE mode: "ah" for x86-64, and
// for an ARM CPU with FEAT_AFP (FPCR.AH, which Dolphin sets there: Apple M4 and later), which
// compute alike; "noah" for an ARM CPU without it (Apple M1 to M3), where AH does nothing and a
// generated NaN gets the other sign.
std::string FloatClass()
{
  // Tests stand in for a machine of another class (a test session never meets a real one).
  if (const std::string forced = Env("ORCA_TEST_FLOAT_CLASS"); !forced.empty())
    return forced;
#if defined(_M_ARM_64) || defined(__aarch64__)
#ifdef __APPLE__
  int afp = 0;
  size_t size = sizeof(afp);
  if (sysctlbyname("hw.optional.arm.FEAT_AFP", &afp, &size, nullptr, 0) != 0)
    afp = 0;
  return afp ? "ah" : "noah";
#else
  return cpu_info.bAFP ? "ah" : "noah";
#endif
#else
  return "ah";
#endif
}

// Project+ (the launcher profile) makes a NaN with fnmsubs as a match starts, so its players must
// share a float class, which the room's hello checks rather than the compatibility key, so the
// friend hears why (the rooms server turns a different key away with a bare 403) (2026-10-02: an M4 Mac and a PC hash a P+ match the same every frame, an
// M4 with AH cleared, as on an M1, diverges at that NaN). Vanilla Brawl never made such a NaN in a
// 3,606-frame scripted match with AH on or off, so it stays open to every machine.
bool FloatClassMatters()
{
  const Profile* profile = ActiveProfile();
  return profile && profile->IsLauncher();
}
}  // namespace

std::string CompatibilityKey()
{
  std::string parts = fmt::format("orca|{}|", Common::GetScmRevGitStr());
  if (const Profile* profile = ActiveProfile())
  {
    parts += fmt::format("{}|{}|", profile->game_id, profile->revision);
    // The data files that reach emulation besides the code: the profile itself, and every game
    // settings INI Dolphin loads for this disc, shipped and the player's own (a local edit there
    // changes what the game runs with, so it must change the key).
    std::vector<std::string> files{File::GetSysDirectory() + "Orca/" + profile->game_id + ".ini",
                                   // Orca's forced game patches (UX/GamePatches.h)
                                   File::GetSysDirectory() + "Orca/" + profile->game_id + ".patches"};
    // A launcher profile runs under two game IDs: its loader's, then the disc's.
    std::vector<std::pair<std::string, u16>> ini_games{{profile->game_id, profile->revision}};
    if (profile->IsLauncher())
    {
      // The running metadata comes from the disc's TMD, whose version can differ from the disc
      // header's revision: hash the INIs for both.
      const SConfig& running = SConfig::GetInstance();
      ini_games = {{profile->boot_game_id, 0},
                   {profile->disc, profile->revision},
                   {running.GetGameID(), running.GetRevision()}};
    }
    for (const auto& [ini_game, ini_revision] : ini_games)
    {
      for (const std::string& name : ConfigLoaders::GetGameIniFilenames(ini_game, ini_revision))
      {
        files.push_back(File::GetSysDirectory() + GAMESETTINGS_DIR DIR_SEP + name);
        files.push_back(File::GetUserPath(D_GAMESETTINGS_IDX) + name);
      }
    }
    // The loader and SD card the boot verified (their hashes are in the profile too; stated here so
    // the key never depends on a profile file staying in step with the check).
    if (profile->IsLauncher())
    {
      parts += fmt::format("launcher={}|disc={}|sd={}|", profile->launcher_hash, profile->disc,
                           profile->sd_image_hash);
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
  // Not the host CPU: JitArm64 and Jit64 compute identical results (12,001 frames of a scripted
  // match hashed the same on an Apple Silicon Mac and an x86-64 Windows PC, 2026-10-01, after the
  // ARM64 emitter and Jit64 ps1 fixes; Project+ too from 2026-10-02, after a launcher boot took a
  // fixed region and JitArm64's sign operations went bitwise), so Mac and PC players meet. FMA,
  // below, is in the key.
  // The universal forced settings that change what the game computes.
  parts += fmt::format(
      "thread={}|fma={}|rtc={}:{}|si={},{},{},{}|efb={}|xfb={}|access={}|bbox={}|save={:016x}|test={}"
      "|sdcard={}:{}",
      Config::Get(Config::MAIN_CPU_THREAD), Config::Get(Config::SESSION_USE_FMA),
      Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE), Config::Get(Config::MAIN_CUSTOM_RTC_VALUE),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(0))),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(1))),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(2))),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(3))),
      Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM),
      Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM),
      Config::Get(Config::GFX_HACK_EFB_ACCESS_ENABLE), Config::Get(Config::GFX_HACK_BBOX_ENABLE),
      GetSeedSaveHash(), TestOverridesActive(), Config::Get(Config::MAIN_WII_SD_CARD),
      Config::Get(Config::MAIN_ALLOW_SD_WRITES));
  // The disc image itself, when the desktop app hashed it (a modified disc with the right game ID
  // and revision would otherwise desync instead of being refused).
  parts += "|disc=" + Env("ORCA_DISC_SHA1");
  // What the in-game UX writes to emulated memory (name tags, mode locks), by its version.
  parts += fmt::format("|ux={}", Events::UXCompatVersion());
  // The room allows 1-64 characters [a-zA-Z0-9._:-].
  return fmt::format("orca1:{:016x}", XXH3_64bits(parts.data(), parts.size()));
}

namespace
{
void StartLocked(Net::RoomOptions options)
{
  options.compatibility = CompatibilityKey();
  if (FloatClassMatters())
    options.platform = FloatClass();
  if (const Profile* profile = ActiveProfile())
  {
    std::string game = profile->game_id;
    Common::ToLower(&game);
    options.mode = "orca-" + game;
  }
  if (!Env("ORCA_NAME").empty())
    options.player_name = Env("ORCA_NAME").substr(0, 32);
  NOTICE_LOG_FMT(NETPLAY, "Orca: joining a YouGame room ({}), compatibility {}",
                 options.joining ? "to join a friend's game" : "as the host",
                 options.compatibility);
  s_room = std::make_shared<Net::YouGameRoom>(std::move(options));
}
}  // namespace

void Start()
{
  std::lock_guard lock(s_mutex);
  if (s_room)
    return;
  Net::RoomOptions options;
  options.room_code = Env("ORCA_ROOM");
  options.joining = Env("ORCA_JOIN") == "1";
  StartLocked(std::move(options));
}

void StartRoom(const std::string& code, bool joining)
{
  std::lock_guard lock(s_mutex);
  if (s_room)
    return;
  Net::RoomOptions options;
  options.room_code = code;
  options.joining = joining;
  options.ask_app_for_room = false;
  StartLocked(std::move(options));
}

std::string SeatName(int seat)
{
  const auto room = Room();
  return room ? room->SeatName(seat) : "";
}

std::string Code()
{
  const auto room = Room();
  return room ? room->Code() : "";
}

namespace
{
std::mutex s_request_mutex;
std::optional<std::string> s_join_request;
bool s_leave_request = false;
}  // namespace

void RequestJoin(const std::string& code)
{
  std::lock_guard lock(s_request_mutex);
  s_join_request = code;
}

std::optional<std::string> TakeJoinRequest()
{
  std::lock_guard lock(s_request_mutex);
  return std::exchange(s_join_request, std::nullopt);
}

void RequestLeave()
{
  std::lock_guard lock(s_request_mutex);
  s_leave_request = true;
}

bool TakeLeaveRequest()
{
  std::lock_guard lock(s_request_mutex);
  return std::exchange(s_leave_request, false);
}

std::optional<bool> Joining()
{
  const auto room = Room();
  return room ? room->Joining() : std::nullopt;
}

Net::Transport* Transport()
{
  const auto room = Room();
  return room.get();
}

int Seat()
{
  const auto room = Room();
  return room ? room->Seat() : -1;
}

std::vector<Net::PeerEvent> TakePeerEvents()
{
  const auto room = Room();
  return room ? room->TakePeerEvents() : std::vector<Net::PeerEvent>{};
}

bool RoomEnded()
{
  const auto room = Room();
  return !room || room->GetState() == Net::RoomState::Ended;
}

bool InRoom()
{
  const auto room = Room();
  return room && room->Connected();
}

bool FreshTicket(bool fresh, std::string* ticket, std::string* store_url, std::string* error)
{
  const auto room = Room();
  if (!room)
  {
    *error = "not in a room";
    return false;
  }
  return room->FreshTicket(fresh, ticket, store_url, error);
}

void DropPeer(int seat, const std::string& reason)
{
  if (const auto room = Room())
    room->DropPeer(seat, reason);
}

bool IsDesync(const std::string& session_error)
{
  return session_error.starts_with("Desync") ||
         session_error.find("changed its input") != std::string::npos ||
         session_error.find("arrived after") != std::string::npos ||
         session_error.find("history changed") != std::string::npos ||
         session_error.starts_with("No local input");
}

bool IsRollbackFailure(const std::string& session_error)
{
  return session_error.starts_with("No snapshot to roll back") ||
         session_error.find("exceeds the limit") != std::string::npos;
}

void BroadcastNames(const std::vector<Net::KeyframeInfo::Name>& names)
{
  if (const auto room = Room())
    room->BroadcastNames(names);
}

namespace
{
// Rooms being left in the background, until their goodbyes are out. Never destroyed: a teardown
// still running when the process exits must not find these gone.
struct Teardowns
{
  std::mutex mutex;
  std::condition_variable done;
  int running = 0;
};
Teardowns& TheTeardowns()
{
  static Teardowns* const teardowns = new Teardowns;
  return *teardowns;
}
}  // namespace

bool RoomsLeaving()
{
  Teardowns& teardowns = TheTeardowns();
  std::lock_guard lock(teardowns.mutex);
  return teardowns.running > 0;
}

void ShutdownInBackground()
{
  std::shared_ptr<Net::YouGameRoom> room;
  {
    std::lock_guard lock(s_mutex);
    room = std::move(s_room);
  }
  if (!room)
    return;
  Teardowns& teardowns = TheTeardowns();
  {
    std::lock_guard lock(teardowns.mutex);
    ++teardowns.running;
  }
  std::thread([room = std::move(room), &teardowns]() mutable {
    room->Leave();
    room.reset();
    std::lock_guard lock(teardowns.mutex);
    --teardowns.running;
    teardowns.done.notify_all();
  }).detach();
}

void OfferKeyframe(int seat, const Net::KeyframeInfo& info)
{
  if (const auto room = Room())
    room->OfferKeyframe(seat, info);
}

namespace
{
std::atomic<bool> s_prepare_join{false};
}

void PrepareJoin()
{
  s_prepare_join.store(true, std::memory_order_relaxed);
}

bool TakePrepareJoin()
{
  return s_prepare_join.exchange(false, std::memory_order_relaxed);
}

int RoundTripMs(u32* sequence)
{
  const auto room = Room();
  return room ? room->RoundTripMs(sequence) : -1;
}

namespace
{
// The room's own code as the app knows it: "taken" is the game's (it opens another room).
std::string AppCode(std::string code)
{
  return code == "taken" ? "network" : code;
}
}  // namespace

std::string RoomErrorCode()
{
  const auto room = Room();
  return room ? AppCode(room->ErrorCode()) : "";
}

bool RoomTaken()
{
  const auto room = Room();
  return room && room->ErrorCode() == "taken";
}

std::string StatusLine()
{
  const auto room = Room();
  return room ? room->Status() : "";
}

bool ReportSessionEnd(const std::string& session_error)
{
  const auto room = Room();
  // Inputs that disagree, or a checksum that does: the games went apart. The friend hears it
  // before this side leaves, so both report the same thing.
  if (IsDesync(session_error))
  {
    if (room)
      room->SetLeaveReason("desync");
    Status::Report("desync", "Your games went out of sync, so the match ended");
    return true;
  }
  // A rollback this machine couldn't make (an engine fault, not the games going apart): it leaves
  // the match and plays on from where it is. Said in its own words, so a log tells it from a
  // desync.
  if (IsRollbackFailure(session_error))
  {
    Status::Report(
        "network",
        fmt::format("Your game couldn't roll back ({}), so you left the match", session_error));
    return true;
  }
  // The session ends when the room does: the room says why, unless this player left. The state
  // first: once Ended, the code is final.
  if (room && room->GetState() == Net::RoomState::Ended)
  {
    if (const std::string code = AppCode(room->ErrorCode()); !code.empty())
      Status::Report(code, room->Status());
    return true;
  }
  if (session_error.empty())
    return true;
  // The emulator side failed (a snapshot that wouldn't save or load, lost frame alignment), or
  // the friend sent something no honest Orca sends.
  const bool from_peer = session_error.find("sent an invalid packet") != std::string::npos;
  if (from_peer)
  {
    Status::Report("network", session_error);
    return true;
  }
  Status::Error("internal", session_error);
  return false;
}

void ReportPeerStalled(std::string_view sentence)
{
  if (const auto room = Room())
    room->SetLeaveReason("stalled");
  Status::Report("peer_left", sentence);
}

void TestDropRoom()
{
  if (const auto room = Room())
    room->TestDropConnection();
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
  // Rooms left in the background just before (a leave, then a quit at once) say their goodbyes
  // too, so a friend hears at once that this player went rather than after its silence limit.
  // Bounded: a teardown that hangs doesn't hold the exit.
  Teardowns& teardowns = TheTeardowns();
  std::unique_lock lock(teardowns.mutex);
  if (!teardowns.done.wait_for(lock, TEARDOWN_WAIT, [&] { return teardowns.running == 0; }))
    WARN_LOG_FMT(NETPLAY, "Orca: {} room(s) still leaving at shutdown", teardowns.running);
}
}  // namespace Orca::Online
