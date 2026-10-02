// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>

#include <fmt/format.h>

#include "AudioCommon/AudioCommon.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/STM/STM.h"
#include "Core/Orca/Branding.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Rollback/OnlineMatch.h"
#include "Core/System.h"

Platform::~Platform() = default;

bool Platform::Init()
{
  return true;
}

void Platform::SetTitle(const std::string& title)
{
}

void Platform::UpdateRunningFlag()
{
  ProcessEmbedCommands();
  EndSoloPauseForDropIn();
  UpdateTitle();
  if (m_shutdown_requested.TestAndClear())
  {
    const auto& system = Core::System::GetInstance();
    const auto ios = system.GetIOS();
    const auto stm = ios ? ios->GetDeviceByName("/dev/stm/eventhook") : nullptr;
    // Orca: an online session stops at once (the power button would reach only this player's game,
    // and the game can't answer it while the session waits for the room).
    if (!Orca::SessionActive() && !m_tried_graceful_shutdown.IsSet() && stm &&
        std::static_pointer_cast<IOS::HLE::STMEventHookDevice>(stm)->HasHookInstalled())
    {
      system.GetProcessorInterface().PowerButton_Tap();
      m_tried_graceful_shutdown.Set();
    }
    else
    {
      m_running.Clear();
    }
  }
}

void Platform::EndSoloPauseForDropIn()
{
  auto& system = Core::System::GetInstance();
  // Only the player's own pause (BeginPause): the CPU also steps at boot, after an HLE reload and
  // on a panic, and none of those is this one to end.
  if (!Orca::SessionActive() || !Rollback::OnlineMatch::Pausing() ||
      Core::GetState(system) != Core::State::Paused || Rollback::OnlineMatch::SoloPauseAllowed())
  {
    return;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Orca: the pause ends for drop-in (a friend arriving, an invite, a join "
                           "or a leave)");
  Core::SetState(system, Core::State::Running);
  if (m_embed.enabled || m_test_commands)
    Embed::Out("state running");
}

void Platform::UpdateTitle()
{
  if (Orca::SessionActive() &&
      Core::GetState(Core::System::GetInstance()) != Core::State::Uninitialized)
  {
    const auto now = std::chrono::steady_clock::now();
    if (now >= m_next_title_check)
    {
      m_next_title_check = now + std::chrono::milliseconds(250);
      ShowTitle(Orca::WindowTitle());
    }
  }

  std::optional<std::string> title;
  {
    std::lock_guard lock(m_title_lock);
    title.swap(m_requested_title);
  }
  if (!title || *title == m_shown_title)
    return;
  m_shown_title = std::move(*title);
  SetTitle(m_shown_title);
}

void Platform::ShowTitle(const std::string& title)
{
  std::lock_guard lock(m_title_lock);
  m_requested_title = title;
}

void Platform::Stop()
{
  m_running.Clear();
}

void Platform::RequestShutdown()
{
  m_shutdown_requested.Set();
}

void Platform::NotifyFirstFrame(int width, int height, double fps)
{
  std::lock_guard lock(m_ready_lock);
  if (m_ready_sent || m_ready_line)
    return;
  m_ready_line = fmt::format("ready {} {} {:.4f}", width, height, fps);
}

void Platform::EmbedTeardown()
{
  if (!m_embed.enabled)
    return;
  EmbedSetFocus(false);
  EmbedSetVisible(false);
}

void Platform::ProcessEmbedCommands()
{
  if (!m_embed.enabled && !m_test_commands)
    return;
  {
    std::optional<std::string> ready;
    {
      std::lock_guard lock(m_ready_lock);
      ready.swap(m_ready_line);
      if (ready)
        m_ready_sent = true;
    }
    if (ready)
    {
      // What this build can do beyond the base protocol, before "ready" (the app answers with
      // "caps <list>"). Outside a session: nothing.
      Orca::Status::PrintCaps();
      Embed::Out(*ready);
    }
  }

  auto& system = Core::System::GetInstance();
  if (m_pause_pending && Core::GetState(system) == Core::State::Running)
  {
    m_pause_pending = false;
    Core::SetState(system, Core::State::Paused);
  }
  const auto report_state = [this, &system] {
    Embed::Out(m_pause_pending || Core::GetState(system) == Core::State::Paused ? "state paused" :
                                                                                  "state running");
  };
  for (const std::string& line : Embed::TakeCommands())
  {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "quit")
    {
      // At once, also outside a session: the app is closing the player (or has died).
      Stop();
    }
    else if (cmd == "rect")
    {
      std::string rest;
      std::getline(in, rest);
      if (const std::optional<Embed::Rect> rect = Embed::ParseRect(rest))
      {
        m_embed.rect = *rect;
        EmbedSetRect(*rect);
      }
    }
    else if (cmd == "pause")
    {
      // A session pauses only while this player's game is alone in it (Core::SetState asks
      // Rollback::OnlineMatch::BeginPause): with a friend, the friend's game keeps going.
      if (Core::GetState(system) == Core::State::Running)
        Core::SetState(system, Core::State::Paused);
      else if (Core::GetState(system) != Core::State::Paused && !Orca::SessionActive())
        m_pause_pending = true;  // still starting
      if (!m_pause_pending && Core::GetState(system) != Core::State::Paused)
        Embed::Out("unsupported pause");
      report_state();
    }
    else if (cmd == "resume")
    {
      m_pause_pending = false;
      if (Core::GetState(system) == Core::State::Paused)
        Core::SetState(system, Core::State::Running);
      report_state();
    }
    else if (cmd == "focus" || cmd == "blur")
    {
      EmbedSetFocus(cmd == "focus");
    }
    else if (cmd == "hide" || cmd == "show")
    {
      EmbedSetVisible(cmd == "show");
      Orca::UX::SetHostVisible(cmd == "show");
    }
    else if (cmd == "volume")
    {
      // 0..1, linear, like the libretro host's gain. Only this run's layer: Dolphin.ini keeps its own.
      float volume = 1.0f;
      if (in >> volume && std::isfinite(volume))
      {
        volume = std::clamp(volume, 0.0f, 1.0f);
        Config::SetCurrent(Config::MAIN_AUDIO_VOLUME, static_cast<int>(std::lround(volume * 100)));
        AudioCommon::UpdateSoundStream(system);
      }
    }
    else if (cmd == "save" || cmd == "load")
    {
      // Save states are the app's libretro feature; Orca keeps the game's own saves in its NAND.
      int slot = 0;
      in >> slot;
      Embed::Out(fmt::format("unsupported {} {}", cmd, slot));
    }
    else if (cmd == "caps")
    {
      // The features the app understands (Orca::Status::CAPS lists ours).
      std::string rest;
      std::getline(in, rest);
      Orca::Status::SetAppCaps(rest);
    }
    else if (cmd == "join")
    {
      // Into a friend's game, from the running one (Orca::Online::ValidCode checks the code).
      std::string code;
      in >> code;
      const bool valid = code.size() >= 6 && code.size() <= 12 &&
                         std::all_of(code.begin(), code.end(), [](char c) {
                           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
                         });
      // Before the app's "caps" line arrives (it answers "orca caps", printed at the first frame), a
      // join waits for it: the emulator thread takes it once "join" is on.
      if (!valid)
        Embed::Out("unsupported join");
      else
        Orca::Online::RequestJoin(code);
    }
    else if (cmd == "leave")
    {
      // As join: taken once the app's "caps" line turns "leave" on.
      Orca::Online::RequestLeave();
    }
    else if (cmd == "delay")
    {
      // This player's input delay: "auto" (adaptive) or a fixed 1 to max_delay (6) frames. Kept
      // for the run and applied while the app's "caps" line has "delay" on; mid-match, from the
      // next frame.
      std::string value;
      in >> value;
      int frames = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), frames);
      if (value == "auto")
        Orca::Online::SetChosenDelay(std::nullopt);
      else if (error == std::errc{} && end == value.data() + value.size() && frames >= 1 &&
               frames <= Orca::Net::Config{}.max_delay)
        Orca::Online::SetChosenDelay(frames);
      else
        Embed::Out("unsupported delay");
    }
    else if (cmd == "ping")
    {
      // The online overlay's ping beside its circle (off until the app asks): "on" the ping, "full"
      // also the delay and rollbacks per second.
      std::string value;
      in >> value;
      if (value == "on" || value == "off" || value == "full")
        Orca::UX::SetStats(value == "on"   ? Orca::UX::Stats::Ping :
                           value == "full" ? Orca::UX::Stats::Full :
                                             Orca::UX::Stats::Off);
    }
    else if (cmd == "perf")
    {
      // The overlay's frame meter, solo too (on, "fps", until the app says): "fps" the frames shown
      // in the last second and the longest, "detailed" also a graph of the last 2 s.
      std::string value;
      in >> value;
      if (value == "off" || value == "fps" || value == "detailed")
        Orca::UX::SetPerf(value == "fps"      ? Orca::UX::Perf::Fps :
                          value == "detailed" ? Orca::UX::Perf::Detailed :
                                                Orca::UX::Perf::Off);
      else
        Embed::Out("unsupported perf");
    }
    else if (cmd == "test-drop-room" && Orca::GetEnv("ORCA_TEST_COMMANDS") == "1")
    {
      // Tests only: the room's connection drops as a network failure would.
      Orca::Online::TestDropRoom();
    }
    else if (cmd == "prepare-join")
    {
      // The host invited a friend who will drop into this game (drop-in join).
      Orca::Online::PrepareJoin();
    }
    else if (cmd == "reset")
    {
      // A session refuses the reset button (it would reach one player's game only).
      if (Orca::SessionActive() || !Core::IsRunning(system))
        Embed::Out("unsupported reset");
      else
        system.GetProcessorInterface().ResetButton_Tap();
    }
    else if (!cmd.empty())
    {
      fmt::print(stderr, "Orca: unknown embed command: {}\n", cmd);
    }
  }
}
