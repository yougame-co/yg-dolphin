// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
class System;
}

// Measurements for rollback performance work. Every one is off unless its variable is set.
//   YG_FRAMETIME=<frame>  wall time between first-pass frame boundaries from that frame on
//   YG_THROTTLE=1         keep Dolphin's throttle in offline harness runs (they run unthrottled)
//   YG_SNAPTIME=1         time snapshot saves (state / MEM1 / MEM2, or state / re-protect for
//                         copy-on-write ones), ring loads, the session port's saves and loads, and
//                         count copy-on-write faults and recorded pages per first-pass frame
//   YG_NTCOPY=1           copy MEM1/MEM2 into snapshots with non-temporal stores (x86-64 only)
//   YG_PAGEDIFF=<frame>   from that frame, count the 4 KB pages of MEM1/MEM2 that differ from 1,
//                         2 and 4 frames earlier, and where in MEM1/MEM2 (1 MB buckets) they are
//   YG_JITCODE_LOG=<path> every instruction word a JIT compile reads that differs from the word
//                         the last compile of that address read ("chg"), or from RAM ("stale": the
//                         emulated instruction cache gave an old line; RAM as a fetch reads it,
//                         with the profile's kept game code, Orca::KeptInstruction), with what
//                         reached that address in between: a guest invalidation (icbi,
//                         dcbst/dcbf), a forced one (a rollback load's restored page), a
//                         whole-cache clear, or nothing. Clears and the JIT's free code space are
//                         logged there too.
// Results are logged (ROLLBACK, notice) when the harness stops at YG_EXIT_AFTER.
namespace Rollback::Diag
{
bool KeepThrottle();
bool SnapTime();

// Called at every first-pass frame boundary.
void OnFirstPassBoundary(Core::System& system, int frame);
void LogSummary();

// MEM1/MEM2 into a snapshot buffer (plain or non-temporal, per YG_NTCOPY).
void CopyRam(std::vector<u8>* dst, const u8* src, std::size_t size);
void AddSnapshotSave(double state_ms, double mem1_ms, double mem2_ms);
void AddCowSave(double state_ms, double protect_ms);
void AddRingLoad(double ms, std::size_t changed_blocks);
void AddPortSave(double ms);
void AddPortLoad(double ms);

// YG_JITCODE_LOG (above): g_jit_code_log is read on every compile and invalidation, one load each.
extern const bool g_jit_code_log;
void JitCodeSetFrame(int frame);
void JitCodeCompiled(u32 block_start, const std::vector<std::pair<u32, u32>>& words);
void JitCodeInvalidated(u32 address, u32 length, bool forced);
void JitCodeCleared(const char* why);
void JitCodeNote(const std::string& line);
// Every word a live JIT block was compiled from that differs from RAM now (as a fetch reads it,
// Orca::KeptInstruction): code the guest rewrote without an invalidation, which the JIT keeps
// running in its old form until something drops the block. Logged as "stalecode" lines, with a
// count.
void JitCodeCensus(const Core::CPUThreadGuard& guard, const char* why);
}  // namespace Rollback::Diag
