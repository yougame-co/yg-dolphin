// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/System.h"

#include <windows.h>
#include <climits>
#include <cstdio>
#include <dwmapi.h>

#include "VideoCommon/Present.h"
#include "resource.h"

namespace
{
class PlatformWin32 final : public Platform
{
public:
  ~PlatformWin32() override;

  bool Init() override;
  void SetTitle(const std::string& string) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;

protected:
  void EmbedSetRect(const Embed::Rect& rect) override;
  void EmbedSetVisible(bool visible) override;
  void EmbedSetFocus(bool focus) override;

private:
  static constexpr TCHAR WINDOW_CLASS_NAME[] = _T("Orca");

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  static bool RegisterRenderWindowClass(bool embedded);
  bool CreateRenderWindow();
  void UpdateWindowPosition();
  void ProcessEvents();

  HWND m_hwnd{};
  // Embed mode: "show" / "hide" (a rect while hidden must not bring the window back).
  bool m_embed_visible = true;
  // "focus" / "blur": the focus the app wants, applied again on "show".
  bool m_embed_focus = false;

  int m_window_x = Config::Get(Config::MAIN_RENDER_WINDOW_XPOS);
  int m_window_y = Config::Get(Config::MAIN_RENDER_WINDOW_YPOS);
  int m_window_width = Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH);
  int m_window_height = Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT);
};

PlatformWin32::~PlatformWin32()
{
  if (m_hwnd)
    DestroyWindow(m_hwnd);
}

bool PlatformWin32::RegisterRenderWindowClass(bool embedded)
{
  WNDCLASSEX wc = {};
  wc.cbSize = sizeof(WNDCLASSEX);
  wc.style = 0;
  wc.lpfnWndProc = WndProc;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.hIcon = LoadIcon(wc.hInstance, IDI_ICON1);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // Embedded, black until the first frame rather than a white box in the app's page.
  wc.hbrBackground = embedded ? static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)) :
                                reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszMenuName = nullptr;
  wc.lpszClassName = WINDOW_CLASS_NAME;
  wc.hIconSm = LoadIcon(wc.hInstance, IDI_ICON1);

  if (!RegisterClassEx(&wc))
  {
    if (embedded)
      fprintf(stderr, "Orca: window registration failed (%lu)\n", GetLastError());
    else
      MessageBox(nullptr, _T("Window registration failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  return true;
}

bool PlatformWin32::CreateRenderWindow()
{
  if (IsEmbedded())
  {
    // A child of the app's window at the player box (client pixels of the parent), like the app's
    // libretro host: clipped and moved with it, sharing its input queue.
    const HWND parent = reinterpret_cast<HWND>(static_cast<uintptr_t>(m_embed.parent));
    const Embed::Rect& r = m_embed.rect;
    // No WM_PARENTNOTIFY: each would be a synchronous call into the app's UI thread.
    m_hwnd = CreateWindowEx(WS_EX_NOPARENTNOTIFY, WINDOW_CLASS_NAME, _T("Orca"),
                            WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, r.x, r.y, r.w, r.h,
                            parent, nullptr, GetModuleHandle(nullptr), this);
    if (!m_hwnd)
    {
      fprintf(stderr, "Orca: CreateWindowEx under the app's window failed (%lu)\n", GetLastError());
      return false;
    }
    m_window_focus = false;
    ShowWindow(m_hwnd, SW_SHOWNA);
    return true;
  }

  m_hwnd = CreateWindowEx(WS_EX_CLIENTEDGE, WINDOW_CLASS_NAME, _T("Orca"), WS_OVERLAPPEDWINDOW,
                          m_window_x < 0 ? CW_USEDEFAULT : m_window_x,
                          m_window_y < 0 ? CW_USEDEFAULT : m_window_y, m_window_width,
                          m_window_height, nullptr, nullptr, GetModuleHandle(nullptr), this);
  if (!m_hwnd)
  {
    MessageBox(nullptr, _T("CreateWindowEx failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  ShowWindow(m_hwnd, SW_SHOW);
  UpdateWindow(m_hwnd);
  return true;
}

bool PlatformWin32::Init()
{
  // Embedded, the rect is in the app's physical pixels (Electron is per-monitor DPI aware): so is
  // this process, or Windows would scale the child window and its swap chain.
  if (IsEmbedded())
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  if (!RegisterRenderWindowClass(IsEmbedded()) || !CreateRenderWindow())
    return false;
  if (IsEmbedded())
    return true;

  // TODO: Enter fullscreen if enabled.
  if (Config::Get(Config::MAIN_FULLSCREEN))
  {
    ProcessEvents();
  }

  if (Config::Get(Config::MAIN_DISABLE_SCREENSAVER))
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

  UpdateWindowPosition();
  return true;
}

void PlatformWin32::SetTitle(const std::string& string)
{
  SetWindowTextW(m_hwnd, UTF8ToWString(string).c_str());
}

void PlatformWin32::MainLoop()
{
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    ProcessEvents();
    UpdateWindowPosition();

    // TODO: Is this sleep appropriate?
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

WindowSystemInfo PlatformWin32::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Windows;
  wsi.render_window = reinterpret_cast<void*>(m_hwnd);
  wsi.render_surface = reinterpret_cast<void*>(m_hwnd);
  return wsi;
}

void PlatformWin32::UpdateWindowPosition()
{
  if (m_window_fullscreen || IsEmbedded())
    return;

  RECT rc = {};
  if (!GetWindowRect(m_hwnd, &rc))
    return;

  m_window_x = rc.left;
  m_window_y = rc.top;
  m_window_width = rc.right - rc.left;
  m_window_height = rc.bottom - rc.top;
}

void PlatformWin32::EmbedSetRect(const Embed::Rect& r)
{
  SetWindowPos(m_hwnd, HWND_TOP, r.x, r.y, r.w, r.h,
               SWP_NOACTIVATE | (m_embed_visible ? SWP_SHOWWINDOW : SWP_NOREDRAW));
}

void PlatformWin32::EmbedSetVisible(bool visible)
{
  m_embed_visible = visible;
  if (!visible && GetFocus() == m_hwnd)
    SetFocus(GetParent(m_hwnd));
  ShowWindow(m_hwnd, visible ? SW_SHOWNA : SW_HIDE);
  if (visible && m_embed_focus)
    SetFocus(m_hwnd);
}

void PlatformWin32::EmbedSetFocus(bool focus)
{
  m_embed_focus = focus;
  if (focus && m_embed_visible)
    SetFocus(m_hwnd);
  else if (!focus && GetFocus() == m_hwnd)
    SetFocus(GetParent(m_hwnd));
}

void PlatformWin32::ProcessEvents()
{
  MSG msg;
  while (PeekMessage(&msg, m_hwnd, 0, 0, PM_REMOVE))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

LRESULT PlatformWin32::WndProc(const HWND hwnd, const UINT msg, const WPARAM wParam,
                               const LPARAM lParam)
{
  PlatformWin32* platform = reinterpret_cast<PlatformWin32*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  switch (msg)
  {
  case WM_NCCREATE:
  {
    platform = static_cast<PlatformWin32*>(reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(platform));
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  case WM_CREATE:
  {
    if (hwnd)
    {
      // Remove rounded corners from the render window on Windows 11
      constexpr DWM_WINDOW_CORNER_PREFERENCE corner_preference = DWMWCP_DONOTROUND;
      DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner_preference,
                            sizeof(corner_preference));
    }
  }
  break;

  case WM_SIZE:
  {
    if (g_presenter)
      g_presenter->ResizeSurface();
  }
  break;

  case WM_KEYDOWN:
    if (platform->IsEmbedded())
    {
      // Once per press, as the libretro host: a held F11 must not toggle full screen over and over.
      if (lParam & (1 << 30))
        break;
      // The page can't see keys while Orca has the focus: the ones that mean something to it.
      if (wParam == VK_ESCAPE)
        Embed::Out("key escape");
      else if (wParam == VK_F11)
        Embed::Out("key fullscreen");
    }
    else if (wParam == VK_ESCAPE)
    {
      platform->RequestShutdown();
    }
    break;

  // Embedded: a click on the game gives it the keyboard; the input gate follows the focus.
  case WM_LBUTTONDOWN:
    if (platform->IsEmbedded())
    {
      platform->m_embed_focus = true;
      SetFocus(hwnd);
    }
    break;

  // Embedded, Alt and F10 are game keys: the default handling would open the app's window menu.
  // Alt+F4 still closes the app.
  case WM_SYSKEYDOWN:
  case WM_SYSKEYUP:
  case WM_SYSCHAR:
    if (platform && platform->IsEmbedded() && wParam != VK_F4)
      return 0;
    return DefWindowProc(hwnd, msg, wParam, lParam);

  // Embedded, the app's window took ours with it: nobody left to show the game to.
  case WM_DESTROY:
    if (platform && platform->IsEmbedded())
      platform->Stop();
    return DefWindowProc(hwnd, msg, wParam, lParam);

  case WM_SETFOCUS:
    if (platform->IsEmbedded())
      platform->m_window_focus = true;
    break;

  case WM_KILLFOCUS:
    if (platform->IsEmbedded())
      platform->m_window_focus = false;
    break;

  case WM_CLOSE:
    // Embedded, only the app ends the game ("quit" or a closed stdin).
    if (!platform->IsEmbedded())
      platform->RequestShutdown();
    break;

  default:
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  return 0;
}
}  // namespace

std::unique_ptr<Platform> Platform::CreateWin32Platform()
{
  return std::make_unique<PlatformWin32>();
}
