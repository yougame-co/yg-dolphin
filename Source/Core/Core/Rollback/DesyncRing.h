// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// DIAGNOSTIC build only (P+ desync study, v13). With YG_DESYNC_MAP=1, a fixed ring of the bulk
// writes into guest RAM that bypass the CPU: EFB copies, DVD DMA, IOS FS file reads and SDIO block
// reads. Each entry holds the session frame at the time of the write, the kind, the guest physical
// address, the length and a 32-bit hash of the bytes written. The desync dump (SessionPort.cpp)
// logs it as "DESYNCMAP ring" lines. With YG_DESYNC_MAP unset every call returns at once and
// nothing is recorded.
//
// Threading: writers run on the CPU, GPU and DVD threads, so the ring is behind a lock. The frame
// is the one the CPU thread last handed to the session port (RingPort::SetPads), so a write on the
// GPU thread (dual core) carries the CPU's frame at the time of the write, not at enqueue.

#pragma once

#include <vector>

#include "Common/CommonTypes.h"

namespace Rollback::DesyncRing
{
enum class Kind : u8
{
  Efb,      // EFB copy written into RAM
  EfbZero,  // texture-only EFB/XFB copy: the placeholder bytes written into RAM instead
  EfbSkip,  // EFB copy to RAM dropped on a frame that skips rendering: RAM keeps the old bytes
  Dvd,      // DVD read DMA into RAM
  IosFs,    // IOS FS file read into an IPC buffer
  Sdio,     // IOS SDIO block read into RAM
};

struct Entry
{
  int frame;
  Kind kind;
  u32 address;
  u32 length;
  u32 hash;
};

// YG_DESYNC_MAP=1, read once.
bool On();
// The session frame about to run (CPU thread).
void SetFrame(int frame);
int Frame();
// `data` may be null (nothing hashed, hash 0).
void Record(Kind kind, u32 address, const u8* data, u32 length);
// `rows` rows of `row_bytes` each, `stride` apart; the length is the whole span.
void RecordRows(Kind kind, u32 address, const u8* data, u32 row_bytes, u32 rows, u32 stride);
// The held entries, oldest first, and how many were recorded since the last reset.
std::vector<Entry> Entries(u64* recorded = nullptr);
void Reset();
const char* KindName(Kind kind);
}  // namespace Rollback::DesyncRing
