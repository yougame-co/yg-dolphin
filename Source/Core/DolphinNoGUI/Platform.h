// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "Common/Flag.h"
#include "Common/WindowSystemInfo.h"
#include "DolphinNoGUI/Embed.h"

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

  // Embed mode (Embed.h): set before Init. The window is drawn inside the parent's.
  void SetEmbed(const Embed::Options& options);
  bool IsEmbedded() const { return m_embed.enabled; }
  // Tests: take the embed protocol's commands on stdin without embedding (ORCA_TEST_COMMANDS=1).
  void EnableTestCommands() { m_test_commands = true; }
  // The first frame was presented: "ready" goes out on the next main loop pass.
  void NotifyFirstFrame(int width, int height, double fps);
  // The main loop ended: hidden and without the focus while emulation stops.
  void EmbedTeardown();

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

  // Orca.app: native dialogs, shown before the game window opens. ChooseFileMacOS returns nullopt
  // when the player cancels.
  static std::optional<std::string> ChooseFileMacOS(const std::string& message);
  static void ShowErrorMacOS(const std::string& title, const std::string& message);
#endif

protected:
  void UpdateRunningFlag();
  // Embed mode, on the main loop's thread: runs the commands read from stdin, prints "ready" once.
  void ProcessEmbedCommands();
  // Orca: a solo pause lasts only while nothing of drop-in needs the game (a friend arriving, an
  // invite, the app's join or leave): then it runs again, and the app hears "state running".
  void EndSoloPauseForDropIn();
  // What the platform does for the window commands. rect: the parent-relative rectangle (units per
  // OS, ORCA.md "Embedding"). visible: the app's "show"/"hide". focus: "focus"/"blur".
  virtual void EmbedSetRect(const Embed::Rect& rect) {}
  virtual void EmbedSetVisible(bool visible) {}
  virtual void EmbedSetFocus(bool focus) {}
  // EmbedSetVisible (and the overlay's host visibility) from the app's show/hide and the rect size.
  void ApplyEmbedVisible();
  // Applies the newest requested title on the main loop's thread. Orca: in a session, also asks
  // for the title again (every 250 ms at most) so it follows the room status.
  void UpdateTitle();

  Common::Flag m_running{true};
  Common::Flag m_shutdown_requested{false};
  Common::Flag m_tried_graceful_shutdown{false};

  // Read by the CPU thread (the input gate, Host_RendererHasFocus).
  std::atomic<bool> m_window_focus = true;
  bool m_window_fullscreen = false;

  Embed::Options m_embed;
  bool m_test_commands = false;

private:
  // Titles are set only on the main loop's thread: on Win32 setting it from the emulation thread
  // waits for the main thread, which could be waiting for this lock.
  std::mutex m_title_lock;
  std::optional<std::string> m_requested_title;
  std::string m_shown_title;
  std::chrono::steady_clock::time_point m_next_title_check{};

  std::mutex m_ready_lock;
  std::optional<std::string> m_ready_line;
  bool m_ready_sent = false;
  // Shown: the app's last "show" / "hide". Too small: the last rect was (Embed::TooSmall), so the
  // game is hidden until a big enough one arrives. Visible only when shown and not too small.
  bool m_embed_shown = true;
  bool m_embed_too_small = false;
  // A "pause" before the game runs (Core::SetState only pauses a running core), applied once it is.
  // Never in a session: it pauses only a game it can see is alone.
  bool m_pause_pending = false;
};
