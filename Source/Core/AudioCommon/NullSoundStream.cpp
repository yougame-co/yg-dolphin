// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioCommon/NullSoundStream.h"

#include <algorithm>
#include <array>
#include <chrono>

#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Orca/Profile.h"

namespace
{
constexpr u32 ORCA_PULL_RATE = 48000;
// Like a shared-mode device: about 10 ms per pull.
constexpr std::chrono::milliseconds ORCA_PULL_PERIOD{10};
constexpr std::size_t ORCA_PULL_MAX_FRAMES = 4096;
}  // namespace

NullSound::~NullSound()
{
  SetRunning(false);
}

bool NullSound::Init()
{
  m_orca_pull = !Orca::GetEnv("ORCA_MIXER_WAV").empty();
  if (m_orca_pull)
  {
    GetMixer()->SetSampleRate(ORCA_PULL_RATE);
    NOTICE_LOG_FMT(ROLLBACK, "Orca mixer: no audio output, but ORCA_MIXER_WAV is set: pulling the "
                             "mixer at {} Hz and discarding it",
                   ORCA_PULL_RATE);
    return true;
  }

  // Make Mixer aware that audio output is disabled.
  GetMixer()->SetSampleRate(0);

  return true;
}

bool NullSound::SetRunning(bool running)
{
  if (!m_orca_pull)
    return true;
  if (running)
  {
    if (!m_orca_pull_running.exchange(true))
      m_orca_pull_thread = std::thread([this] { OrcaPullLoop(); });
  }
  else if (m_orca_pull_running.exchange(false))
  {
    if (m_orca_pull_thread.joinable())
      m_orca_pull_thread.join();
  }
  return true;
}

void NullSound::SetVolume(int volume)
{
}

void NullSound::OrcaPullLoop()
{
  Common::SetCurrentThreadName("Orca null audio pull");
  using OrcaClock = std::chrono::steady_clock;
  std::array<s16, ORCA_PULL_MAX_FRAMES * 2> buffer;
  const OrcaClock::time_point start = OrcaClock::now();
  u64 pulled = 0;
  OrcaClock::time_point next = start;
  while (m_orca_pull_running.load(std::memory_order_relaxed))
  {
    next += ORCA_PULL_PERIOD;
    std::this_thread::sleep_until(next);
    // Pull what a real-time device would have consumed by now, whatever the sleep's granularity.
    const double elapsed = std::chrono::duration<double>(OrcaClock::now() - start).count();
    const u64 due = static_cast<u64>(elapsed * ORCA_PULL_RATE);
    // After a long suspension (more than a second behind), restart the count instead of bursting.
    if (due > pulled + ORCA_PULL_RATE)
      pulled = due - ORCA_PULL_MAX_FRAMES;
    while (pulled < due)
    {
      const std::size_t frames = static_cast<std::size_t>(
          std::min<u64>(due - pulled, ORCA_PULL_MAX_FRAMES));
      GetMixer()->Mix(buffer.data(), frames);
      pulled += frames;
    }
    if (OrcaClock::now() > next + std::chrono::seconds(1))
      next = OrcaClock::now();
  }
}
