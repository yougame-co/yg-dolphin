// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Copy-on-write rollback snapshots of guest RAM (MEM1 and MEM2).
//
// A snapshot write-protects every page of guest RAM in every host mapping of it: the RAM views
// host code uses, the fastmem arena's physical and logical views the JIT uses, and any page-table
// mapping. The first write to a page after a snapshot faults, whoever makes it (JIT code, the
// interpreter, a DMA's memcpy, IOS): the fault handler (MemTools.cpp) records the page's contents in
// the newest snapshot's undo log (UndoLog.h), makes the page writable in the view that faulted, and
// resumes the write, which runs again from the start. Later writes to that page cost nothing until
// the next snapshot. A snapshot therefore costs what the frames after it changed, not 96 MB.
//
// A fault costs far more than copying a page, and a game writes much the same pages every frame:
// pages that keep changing stay writable across snapshots, their pre-images copied by the snapshot
// itself ("hot pages" in Cow.cpp). Either way, every page is read-only in every view or has its
// pre-image in the newest log.
//
// Pre-images are copied from, and restored into, a private mapping of RAM that nothing else uses
// and that is never protected. RAM is tracked in host pages (4 KB on x86-64, 16 KB on Apple
// Silicon).
//
// Writes the kernel makes into guest RAM (a read() into an IOS buffer) do not fault: they fail.
// Code that reads a file or socket straight into guest RAM calls PrepareHostWrite first.
//
// One snapshot ring at a time owns the tracker. The CPU thread drives it; the fault handler may run
// on any thread (the Mach exception thread on macOS), so the state is behind a lock.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace Rollback::Cow
{
// A tracked area of guest physical memory (MEM1, MEM2) and its private, never-protected mapping.
struct Area
{
  u8* alias;
  u32 physical_address;
  u32 size;
};

// One host mapping of guest physical memory. Views that are not writeable (read-only page-table
// mappings) are left alone: Dolphin's own fault handling owns writes through them.
struct View
{
  u8* base;
  u32 physical_address;
  u32 size;
  bool writeable;
};

// The host's protection granularity (the page size the logs work in).
std::size_t PageSize();

// Starts tracking `areas` through `views` for `owner` (a snapshot ring): every page becomes
// read-only in every writeable view. False if another owner tracks, the platform has no fault
// handler, or the layout does not fit (unaligned, or more than 64 views).
bool Arm(const void* owner, const std::vector<Area>& areas, const std::vector<View>& views);
// Arm over MEM1 and MEM2 of the running machine.
bool ArmForSystem(Core::System& system, const void* owner);
// Stops tracking for `owner` (nothing for anyone else): RAM is writable everywhere again and every
// log is dropped.
void Disarm(const void* owner);
bool IsArmedFor(const void* owner);

// Takes a snapshot: protects again the pages written since the previous one and opens an empty
// undo log. Returns the snapshot's id (never 0).
u64 Snapshot();
bool Has(u64 id);
// Puts RAM back as it was at snapshot `id`. Calls changed(physical_address, 4096) for each 4 KB
// block whose bytes it changed. Snapshots newer than `id` are dropped and `id` is the newest again,
// with an empty log. False if `id` is not held.
bool Restore(u64 id, const std::function<void(u32 physical_address, u32 length)>& changed);
// Forgets snapshot `id` (its pre-images merge into the next older snapshot's log).
void Drop(u64 id);
// RamChecksum (Rollback.h) of RAM as it was at snapshot `id`, rebuilt from live RAM and the logs.
std::optional<u64> Checksum(u64 id);

// The fault handlers call this first. True when `address` is a write-protected page of tracked RAM:
// its pre-image is recorded and it is writable again, so the faulting write can run again.
bool HandleFault(uintptr_t address);
// Before host code writes [ptr, ptr + size) of guest RAM through a system call (which fails on a
// protected page instead of faulting): records and unprotects those pages.
void PrepareHostWrite(const void* ptr, std::size_t size);

// Memmap calls this when the host mappings of guest RAM change.
void OnMappingsChanged(Core::System& system);
// Stops tracking, whoever owns it: RAM writable in every view again, every snapshot dropped. Before
// the mappings go away (Memmap) and before the fault handler does (the CPU thread exits).
void StopTracking();

struct Counters
{
  u64 faults = 0;          // faults handled (a page written through another view faults again)
  u64 pages_recorded = 0;  // pre-images recorded by faults
  u64 pages_copied = 0;    // pre-images of hot pages copied up front by snapshots and loads
  u64 pages_reprotected = 0;
  u64 remaps = 0;          // mapping changes while armed
};
Counters GetCounters();
}  // namespace Rollback::Cow
