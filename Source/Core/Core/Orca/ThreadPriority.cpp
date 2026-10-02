// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Orca::PrioritizeThread (Profile.h): interactive scheduling for the threads an input's way through
// Orca runs on. Kept apart from Profile.cpp so the platform headers it needs stay out of there.

#include "Core/Orca/Profile.h"

#if defined(_WIN32)
#include <windows.h>
#include <processthreadsapi.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

#include "Common/Logging/Log.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Rollback/Diag.h"
#include "Core/Rollback/Harness.h"

namespace Orca
{
namespace
{
const char* Name(LatencyThread thread)
{
  switch (thread)
  {
  case LatencyThread::Cpu:
    return "CPU";
  case LatencyThread::Room:
    return "room";
  case LatencyThread::Controllers:
    return "controller stream";
  }
  return "?";
}

bool Wanted(LatencyThread thread)
{
  if (!SessionActive())
    return false;
  const std::string knob = GetEnv("ORCA_THREAD_QOS");
  if (knob == "0")
    return false;
  if (knob == "1" || thread != LatencyThread::Cpu)
    return true;
  // A scripted harness run goes as fast as it can unless something paces it (an online match, or
  // YG_THROTTLE): left as it was, it can't hold a core at interactive priority for minutes.
  return !Rollback::Harness::Active() || Online::Enabled() || Rollback::Diag::KeepThrottle();
}
}  // namespace

void PrioritizeThread(LatencyThread thread)
{
  if (!Wanted(thread))
    return;
  bool ok = false;
#if defined(_WIN32)
  // The CPU thread runs flat out between throttles, a notch up; the two network threads mostly
  // wait, and should run the moment their data arrives. Power throttling (EcoQoS) off for the
  // thread as Dolphin already does for the process (Timer::IncreaseResolution): Orca's window is a
  // child of the app's, so Windows never counts Orca as the foreground process.
  ok = SetThreadPriority(GetCurrentThread(), thread == LatencyThread::Cpu ?
                                                 THREAD_PRIORITY_ABOVE_NORMAL :
                                                 THREAD_PRIORITY_HIGHEST) != 0;
  THREAD_POWER_THROTTLING_STATE throttling{};
  throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
  throttling.StateMask = 0;
  SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof(throttling));
#elif defined(__APPLE__)
  ok = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
#else
  return;  // Orca ships for macOS and Windows only
#endif
  NOTICE_LOG_FMT(CORE, "Orca: {} thread at interactive priority{}", Name(thread),
                 ok ? "" : " (refused)");
}
}  // namespace Orca
