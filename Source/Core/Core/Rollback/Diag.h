// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

// Measurements for rollback performance work. Every one is off unless its variable is set.
//   YG_FRAMETIME=<frame>  wall time between first-pass frame boundaries from that frame on
//   YG_THROTTLE=1         keep Dolphin's throttle in offline harness runs (they run unthrottled)
//   YG_SNAPTIME=1         time snapshot saves (state / MEM1 / MEM2) and the session port's saves
//                         and loads
//   YG_NTCOPY=1           copy MEM1/MEM2 into snapshots with non-temporal stores (x86-64 only)
//   YG_PAGEDIFF=<frame>   from that frame, count the 4 KB pages of MEM1/MEM2 that differ from 1,
//                         2 and 4 frames earlier, and where in MEM1/MEM2 (1 MB buckets) they are
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
void AddPortSave(double ms);
void AddPortLoad(double ms);
}  // namespace Rollback::Diag
