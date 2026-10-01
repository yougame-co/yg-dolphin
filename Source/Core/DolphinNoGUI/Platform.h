// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "Common/Flag.h"
#include "Common/WindowSystemInfo.h"

class Platform
{
public:
  virtual ~Platform();

  bool IsRunning() const { return m_running.IsSet(); }
  bool IsWindowFocused() const { return m_window_focus; }
  bool IsWindowFullscreen() const { return m_window_fullscreen; }

  virtual bool Init();
  virtual void SetTitle(const std::string& title);
  virtual void MainLoop() = 0;

  virtual WindowSystemInfo GetWindowSystemInfo() const = 0;

  // Asks for this window title; the main loop applies it (from any thread).
  void ShowTitle(const std::string& title);

  // Requests a graceful shutdown, from SIGINT/SIGTERM.
  void RequestShutdown();

  // Request an immediate shutdown.
  void Stop();

  static std::unique_ptr<Platform> CreateHeadlessPlatform();
#ifdef HAVE_X11
  static std::unique_ptr<Platform> CreateX11Platform();
#endif

#ifdef __linux__
  static std::unique_ptr<Platform> CreateFBDevPlatform();
#endif

#ifdef _WIN32
  static std::unique_ptr<Platform> CreateWin32Platform();
#endif

#ifdef __APPLE__
  static std::unique_ptr<Platform> CreateMacOSPlatform();
#endif

protected:
  void UpdateRunningFlag();
  // Applies the newest requested title on the main loop's thread. Orca: in a session, also asks
  // for the title again (every 250 ms at most) so it follows the room status.
  void UpdateTitle();

  Common::Flag m_running{true};
  Common::Flag m_shutdown_requested{false};
  Common::Flag m_tried_graceful_shutdown{false};

  bool m_window_focus = true;  // Should be made atomic if actually implemented
  bool m_window_fullscreen = false;

private:
  // Titles are set only on the main loop's thread: on Win32 setting it from the emulation thread
  // waits for the main thread, which could be waiting for this lock.
  std::mutex m_title_lock;
  std::optional<std::string> m_requested_title;
  std::string m_shown_title;
  std::chrono::steady_clock::time_point m_next_title_check{};
};
