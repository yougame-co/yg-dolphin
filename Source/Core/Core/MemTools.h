// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace EMM
{
void InstallExceptionHandler();
void UninstallExceptionHandler();
bool IsExceptionHandlerSupported();
// Copy-on-write rollback snapshots (Rollback/Cow.h) need write faults on guest RAM handled on every
// thread. On macOS, InstallExceptionHandler covers only the thread that called it (a Mach thread
// exception port); this adds a process-wide signal handler that takes those faults on any other
// thread and passes everything else on. Elsewhere the handler is process-wide already: nothing to do.
void InstallCowFallbackHandler();

// DIAGNOSTIC build only (Windows x64): what the write-fault handler saw of the vector registers.
struct CowVectorAudit
{
  bool audit = false;
  bool clobber = false;
  unsigned long long faults = 0;
  unsigned long long context_has_avx = 0;
  unsigned long long context_differs = 0;
  unsigned long long handler_changed = 0;
  unsigned long long avx512_in_use = 0;
  unsigned long long context_no_avx512 = 0;
  unsigned long long wide_stores = 0;
  unsigned long long wide_no_avx = 0;
  unsigned long long evex_stores = 0;
  unsigned long long evex_no_avx512 = 0;
};
CowVectorAudit GetCowVectorAudit();
// DIAGNOSTIC build only (Windows x64): the first faulting 256-bit stores, where they came from.
struct CowWideSample
{
  unsigned long long rip = 0;
  unsigned long long ret = 0;  // the 8 bytes at RSP (a leaf copy's return address)
  unsigned long long address = 0;
  unsigned long tid = 0;
  bool context_avx = false;
  bool host_copy = false;  // inside MemoryManager::CopyToEmu or Memset
  unsigned char bytes[16] = {};
};
// Copies out samples not taken before; returns how many.
unsigned TakeCowWideSamples(CowWideSample* out, unsigned max);
// Marks the calling thread as inside a host copy into guest RAM.
void SetHostCopyActive(bool active);
}  // namespace EMM
