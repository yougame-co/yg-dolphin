// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include "Core/Core.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/STM/STM.h"
#include "Core/Orca/Branding.h"
#include "Core/Orca/Profile.h"
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
