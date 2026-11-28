// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <atomic>
#include <cstddef>
#include <span>
#include "Common/CommonTypes.h"

namespace Broadcast
{
// Only the product replay player's lifetime enables media. An env variable alone never does.
void StartReplay();
void StopReplay();
// A live session's feed, switched by the app's stdin "broadcast on" / "broadcast off" so frames are
// read back only while someone watches. The first "on" opens ORCA_BROADCAST_SINK and keeps it
// mapped; "off" stops every readback and marks the ring stopped. Inert in product playback, whose
// replay lifetime owns the sink.
enum class LiveResult
{
  On,
  Off,
  NoSink,
  Failed,
  Replay,
};
LiveResult StartLive();
LiveResult StopLive();
// Audio backend lifetime: zero output rate means no mixer callbacks.
void SetSilentAudio(bool enabled);
bool SilentAudioRunning();
inline std::atomic<bool> s_enabled{false};
inline bool Active() { return s_enabled.load(std::memory_order_acquire); }
u64 NowMicros();
void Video(const u8* rgba, size_t stride, u64 timestamp);
void VideoDrop();
// Sound stream thread, after every source has been mixed. Resamples the final stereo mix to 48k.
void Audio(const s16* samples, size_t frames, u32 rate);
}  // namespace Broadcast
