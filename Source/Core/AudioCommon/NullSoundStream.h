// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <thread>

#include "AudioCommon/SoundStream.h"

class NullSound final : public SoundStream
{
public:
  ~NullSound() override;
  bool Init() override;
  bool SetRunning(bool running) override;
  void SetVolume(int volume) override;

  static bool IsValid() { return true; }

private:
  // Orca diag: with ORCA_MIXER_WAV set, "No Audio Output" still pulls the mixer at the real-time
  // rate of a 48 kHz device and discards what it gets, so the mixer's queue, gap filling and
  // the WAV tap behave as they would with a real backend.
  void OrcaPullLoop();
  bool m_orca_pull = false;
  std::atomic<bool> m_orca_pull_running{false};
  std::thread m_orca_pull_thread;
};
