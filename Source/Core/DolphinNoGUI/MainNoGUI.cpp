// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include <OptionParser.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/MsgHandler.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/Host.h"
#include "Core/Orca/Branding.h"
#include "Core/Orca/Disc.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"
#include "Core/System.h"

#include "UICommon/CommandLineParse.h"
#ifdef USE_DISCORD_PRESENCE
#include "UICommon/DiscordPresence.h"
#endif
#include "UICommon/UICommon.h"

static std::unique_ptr<Platform> s_platform;

static void signal_handler(int)
{
  constexpr char message[] = "A signal was received. A second signal will force Orca to stop.\n";
#ifdef _WIN32
  puts(message);
#else
  if (write(STDERR_FILENO, message, sizeof(message)) < 0)
  {
  }
#endif

  s_platform->RequestShutdown();
}

#ifdef _WIN32
// Alerts show a message box, as with the default handler, but name Orca in a session.
static bool MsgAlertHandler(const char* caption, const char* text, bool yes_no,
                            Common::MsgType style)
{
  UINT window_style = MB_ICONINFORMATION;
  if (style == Common::MsgType::Question)
    window_style = MB_ICONQUESTION;
  if (style == Common::MsgType::Warning)
    window_style = MB_ICONWARNING;

  return IDYES == MessageBox(nullptr, UTF8ToTStr(Orca::BrandText(text)).c_str(),
                             UTF8ToTStr(caption).c_str(),
                             window_style | (yes_no ? MB_YESNO : MB_OK));
}
#else
// Alerts go to stderr, as with the default handler, but name Orca in a session.
static bool MsgAlertHandler(const char*, const char* text, bool, Common::MsgType)
{
  fmt::print(stderr, "{}\n", Orca::BrandText(text));
  // No to any question, like the default handler.
  return false;
}
#endif

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}

void Host_PPCSymbolsChanged()
{
}

void Host_PPCBreakpointsChanged()
{
}

bool Host_UIBlocksControllerState()
{
  return false;
}

void Host_Message(const HostMessageID id)
{
  if (id == HostMessageID::WMUserStop)
    s_platform->Stop();
}

void Host_UpdateTitle(const std::string& title)
{
  s_platform->ShowTitle(title);
}

void Host_UpdateDisasmDialog()
{
}

void Host_JitCacheInvalidation()
{
}

void Host_JitProfileDataWiped()
{
}

void Host_RequestRenderWindowSize(int width, int height)
{
}

bool Host_RendererHasFocus()
{
  return s_platform->IsWindowFocused();
}

bool Host_RendererHasFullFocus()
{
  // Mouse capturing isn't implemented
  return Host_RendererHasFocus();
}

bool Host_RendererIsFullscreen()
{
  return s_platform->IsWindowFullscreen();
}

bool Host_TASInputHasFocus()
{
  return false;
}

void Host_YieldToUI()
{
}

void Host_TitleChanged()
{
#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif
}

void Host_UpdateDiscordClientID(const std::string& client_id)
{
#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateClientID(client_id);
#endif
}

bool Host_UpdateDiscordPresenceRaw(const std::string& details, const std::string& state,
                                   const std::string& large_image_key,
                                   const std::string& large_image_text,
                                   const std::string& small_image_key,
                                   const std::string& small_image_text,
                                   const int64_t start_timestamp, const int64_t end_timestamp,
                                   const int party_size, const int party_max)
{
#ifdef USE_DISCORD_PRESENCE
  return Discord::UpdateDiscordPresenceRaw(details, state, large_image_key, large_image_text,
                                           small_image_key, small_image_text, start_timestamp,
                                           end_timestamp, party_size, party_max);
#else
  return false;
#endif
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core)
{
  return nullptr;
}

static std::unique_ptr<Platform> GetPlatform(const optparse::Values& options)
{
  std::string platform_name = static_cast<const char*>(options.get("platform"));

#if HAVE_X11
  if (platform_name == "x11" || platform_name.empty())
    return Platform::CreateX11Platform();
#endif

#ifdef __linux__
  if (platform_name == "fbdev" || platform_name.empty())
    return Platform::CreateFBDevPlatform();
#endif

#ifdef _WIN32
  if (platform_name == "win32" || platform_name.empty())
    return Platform::CreateWin32Platform();
#endif
#ifdef __APPLE__
  if (platform_name == "macos" || platform_name.empty())
    return Platform::CreateMacOSPlatform();
#endif

  if (platform_name == "headless" || platform_name.empty())
    return Platform::CreateHeadlessPlatform();

  return nullptr;
}

#ifdef ORCA_MACOS_BUNDLE
// Orca.app, started with no disc: the one remembered in <user>/Config/Orca.ini if it is still there
// and still right, else the player picks one (asked again after a wrong one). nullopt, with the
// "orca error" line printed, when they give up.
static std::optional<std::string> ChooseSessionDisc(const std::string& user_directory)
{
  const std::string ini_path = user_directory + DIR_SEP "Config" DIR_SEP "Orca.ini";
  Common::IniFile ini;
  ini.Load(ini_path);
  auto* const disc_section = ini.GetOrCreateSection("Disc");
  std::string remembered;
  disc_section->Get("Path", &remembered);
  if (!remembered.empty() && Orca::CheckDisc(remembered, Orca::APP_GAME_ID).ok)
    return remembered;

  std::optional<Orca::DiscCheck> refused;
  while (true)
  {
    const std::optional<std::string> path =
        Platform::ChooseFileMacOS(std::string(Orca::APP_DISC_PROMPT));
    if (!path)
    {
      if (refused)
        Orca::Status::Error(refused->code, refused->sentence);
      else
        Orca::Status::Error("cancelled", "No disc image was chosen.");
      return std::nullopt;
    }
    Orca::DiscCheck check = Orca::CheckDisc(*path, Orca::APP_GAME_ID);
    if (check.ok)
    {
      disc_section->Set("Path", *path);
      File::CreateFullPath(ini_path);
      ini.Save(ini_path);
      return path;
    }
    fmt::print(stderr, "Orca: {}: {}\n", *path, check.sentence);
    // A broken profile isn't the disc's fault: asking again wouldn't help.
    if (check.code != "disc_revision")
    {
      Orca::Status::Error(check.code, check.sentence);
      return std::nullopt;
    }
    Platform::ShowErrorMacOS("Orca can't play that disc", check.sentence);
    refused = std::move(check);
  }
}
#endif

#ifdef _WIN32
#define main app_main
#endif

int main(const int argc, char* argv[])
{
#ifdef ORCA_MACOS_BUNDLE
  // Orca.app runs sessions: the YouGame app starts it with no arguments. ORCA_SESSION=0 (or any value
  // already set) wins. Until the canonical save exists (O5), app sessions start from an empty NAND.
  setenv("ORCA_SESSION", "1", 0);
  Orca::SetSeedSessionSave(false);
#endif

  const auto parser =
      CommandLineParse::CreateParser(CommandLineParse::ParserOptions::OmitGUIOptions);
  parser->add_option("-p", "--platform")
      .action("store")
      .help("Window platform to use [%choices]")
      .choices({"headless"
#ifdef __linux__
                ,
                "fbdev"
#endif
#if HAVE_X11
                ,
                "x11"
#endif
#ifdef _WIN32
                ,
                "win32"
#endif
#ifdef __APPLE__
                ,
                "macos"
#endif
      });

  optparse::Values& options = CommandLineParse::ParseArguments(parser.get(), argc, argv);
  std::vector<std::string> args = parser->args();

  std::optional<std::string> save_state_path;
  if (options.is_set("save_state"))
  {
    save_state_path = static_cast<const char*>(options.get("save_state"));
  }

  std::string user_directory;
  if (options.is_set("user"))
    user_directory = static_cast<const char*>(options.get("user"));
#ifdef ORCA_MACOS_BUNDLE
  // Its own user directory, never Dolphin's.
  else if (const char* home = std::getenv("HOME"); home && *home)
    user_directory = std::string(home) + "/Library/Application Support/Orca";
#endif

  std::unique_ptr<BootParameters> boot;
  bool game_specified = false;
  if (options.is_set("exec"))
  {
    const std::list<std::string> paths_list = options.all("exec");
    const std::vector<std::string> paths{std::make_move_iterator(std::begin(paths_list)),
                                         std::make_move_iterator(std::end(paths_list))};
    boot = BootParameters::GenerateFromFile(
        paths, BootSessionData(save_state_path, DeleteSavestateAfterBoot::No));
    game_specified = true;
  }
  else if (options.is_set("nand_title"))
  {
    const std::string hex_string = static_cast<const char*>(options.get("nand_title"));
    if (hex_string.length() != 16)
    {
      fprintf(stderr, "Invalid title ID\n");
      parser->print_help();
      return 1;
    }
    const u64 title_id = std::stoull(hex_string, nullptr, 16);
    boot = std::make_unique<BootParameters>(BootParameters::NANDTitle{title_id});
  }
  else if (args.size())
  {
    boot = BootParameters::GenerateFromFile(
        args.front(), BootSessionData(save_state_path, DeleteSavestateAfterBoot::No));
    args.erase(args.begin());
    game_specified = true;
  }
#ifdef ORCA_MACOS_BUNDLE
  else if (Orca::SessionActive() && !user_directory.empty())
  {
    const std::optional<std::string> disc = ChooseSessionDisc(user_directory);
    if (!disc)
      return Orca::Status::Finish();
    boot = BootParameters::GenerateFromFile(
        *disc, BootSessionData(save_state_path, DeleteSavestateAfterBoot::No));
    game_specified = true;
  }
#endif
  else
  {
    parser->print_help();
    return 0;
  }

  Common::RegisterMsgAlertHandler(MsgAlertHandler);

  s_platform = GetPlatform(options);
  if (!s_platform || !s_platform->Init())
  {
    fprintf(stderr, "No platform found, or failed to initialize.\n");
    return 1;
  }

  const WindowSystemInfo wsi = s_platform->GetWindowSystemInfo();

  UICommon::SetUserDirectory(user_directory);
  UICommon::Init();
#ifdef ORCA_MACOS_BUNDLE
  // The local controller is port 1: until the player has their own mapping, the keyboard and the
  // first SDL gamepad (Sys/Orca/Input/GCPadNew.ini).
  if (Orca::SessionActive())
  {
    const std::string pad_ini = File::GetUserPath(D_CONFIG_IDX) + "GCPadNew.ini";
    if (!File::Exists(pad_ini) &&
        !(File::CreateFullPath(pad_ini) &&
          File::Copy(File::GetSysDirectory() + "Orca/Input/GCPadNew.ini", pad_ini)))
    {
      fmt::print(stderr, "Orca: couldn't write the default controller mapping to {}\n", pad_ini);
    }
  }
#endif
  UICommon::InitControllers(wsi);

  Common::ScopeGuard ui_common_guard([] {
    UICommon::ShutdownControllers();
    UICommon::Shutdown();
  });

  if (save_state_path && !game_specified)
  {
    fprintf(stderr, "A save state cannot be loaded without specifying a game to launch.\n");
    return 1;
  }

  auto core_state_changed_hook = Core::AddOnStateChangedCallback([](const Core::State state) {
    if (state == Core::State::Uninitialized)
      s_platform->Stop();
  });

#ifdef _WIN32
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
#else
  // Shut down cleanly on SIGINT and SIGTERM
  struct sigaction sa;
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_RESETHAND;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
#endif

  DolphinAnalytics::Instance().ReportDolphinStart("nogui");

  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot), wsi))
  {
    fprintf(stderr, "Could not boot the specified file\n");
    Orca::Status::Finish();
    return 1;
  }

#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif

  s_platform->MainLoop();
  Core::Stop(Core::System::GetInstance());

  Core::Shutdown(Core::System::GetInstance());
  s_platform.reset();

  // In an Orca session the "orca error" line explains a non-zero exit.
  return Orca::Status::Finish();
}

#ifdef _WIN32
int wmain(int, wchar_t*[], wchar_t*[])
{
  std::vector<std::string> args = Common::CommandLineToUtf8Argv(GetCommandLineW());
  const int argc = static_cast<int>(args.size());
  std::vector<char*> argv(args.size());
  for (size_t i = 0; i < args.size(); ++i)
    argv[i] = args[i].data();

  return main(argc, argv.data());
}

#undef main
#endif
